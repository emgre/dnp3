/*
 * Licensed to Green Energy Corp (www.greenenergycorp.com) under one or
 * more contributor license agreements. See the NOTICE file distributed
 * with this work for additional information regarding copyright ownership.
 * Green Energy Corp licenses this file to you under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This project was forked on 01/01/2013 by Automatak, LLC and modifications
 * may have been made to this file. Automatak, LLC licenses these modifications
 * to you under the terms of the License.
 */
#include "LinkLayer.h"

#include "opendnp3/link/PriLinkLayerStates.h"
#include "opendnp3/link/SecLinkLayerStates.h"
#include "opendnp3/link/LinkFrame.h"
#include "opendnp3/link/ILinkTx.h"

#include "opendnp3/ErrorCodes.h"


using namespace openpal;

namespace opendnp3
{

LinkLayer::LinkLayer(openpal::Logger logger, openpal::IExecutor& executor, IUpperLayer& upper, opendnp3::ILinkListener& linkListener, const LinkConfig& config) :
	logger(logger),
	config(config),
	pSegments(nullptr),
	txMode(LinkTransmitMode::Idle),
	numRetryRemaining(0),
	pExecutor(&executor),
	rspTimeoutTimer(executor),
	keepAliveTimer(executor),
	nextReadFCB(false),
	nextWriteFCB(false),
	isOnline(false),
	isRemoteReset(false),
	keepAliveTimeout(false),
	lastMessageTimestamp(executor.GetTime()),
	pRouter(nullptr),
	pPriState(&PLLS_Idle::Instance()),
	pSecState(&SLLS_NotReset::Instance()),
	pListener(&linkListener),
	pUpperLayer(&upper)
{}

void LinkLayer::Send(ITransportSegment& segments)
{
	if(SetTxSegment(segments))
	{
		TryStartTransmission();
	}
}

bool LinkLayer::OnLowerLayerUp()
{
	if (this->isOnline)
	{
		SIMPLE_LOG_BLOCK(logger, flags::ERR, "Layer already online");
		return false;
	}

	this->isOnline = true;

	auto now = this->pExecutor->GetTime();
	this->lastMessageTimestamp = now; // no reason to trigger a keep-alive until we've actually expired
	MonotonicTimestamp expiration(now.milliseconds + config.KeepAliveTimeout.GetMilliseconds());
	this->StartKeepAliveTimer(MonotonicTimestamp(now.milliseconds + config.KeepAliveTimeout.GetMilliseconds()));

	pListener->OnStateChange(opendnp3::LinkStatus::UNRESET);

	if (pUpperLayer)
	{
		this->pUpperLayer->OnLowerLayerUp();
	}

	return true;
}

bool LinkLayer::OnLowerLayerDown()
{
	if (!isOnline)
	{
		SIMPLE_LOG_BLOCK(logger, flags::ERR, "Layer is not online");
		return false;
	}

	isOnline = false;
	keepAliveTimeout = false;
	isRemoteReset = false;
	pSegments = nullptr;
	txMode = LinkTransmitMode::Idle;
	pendingPriTx.Clear();
	pendingSecTx.Clear();

	rspTimeoutTimer.Cancel();
	keepAliveTimer.Cancel();

	pPriState = &PLLS_Idle::Instance();
	pSecState = &SLLS_NotReset::Instance();

	pListener->OnStateChange(opendnp3::LinkStatus::UNRESET);

	if (pUpperLayer)
	{
		this->pUpperLayer->OnLowerLayerDown();
	}

	return true;
}

bool LinkLayer::SetTxSegment(ITransportSegment& segments)
{
	if (!this->isOnline)
	{
		SIMPLE_LOG_BLOCK(this->logger, flags::ERR, "Layer is not online");
		return false;
	}

	if (this->pSegments)
	{
		SIMPLE_LOG_BLOCK(this->logger, flags::ERR, "Already transmitting a segment");
		return false;
	}

	this->pSegments = &segments;
	return true;
}

bool LinkLayer::OnTransmitResult(bool success)
{
	if (this->txMode == LinkTransmitMode::Idle)
	{
		SIMPLE_LOG_BLOCK(this->logger, flags::ERR, "Unknown transmission callback");
		return false;
	}

	auto isPrimary = (this->txMode == LinkTransmitMode::Primary);
	this->txMode = LinkTransmitMode::Idle;

	// before we dispatch the transmit result, give any pending transmissions access first
	this->TryPendingTx(this->pendingSecTx, false);
	this->TryPendingTx(this->pendingPriTx, true);

	// now dispatch the completion event to the correct state handler
	if (isPrimary)
	{
		this->pPriState = &this->pPriState->OnTransmitResult(*this, success);
	}
	else
	{
		this->pSecState = &this->pSecState->OnTransmitResult(*this, success);
	}

	TryStartTransmission();

	return true;
}

openpal::RSlice LinkLayer::FormatPrimaryBufferWithConfirmed(const openpal::RSlice& tpdu, bool FCB)
{
	auto dest = this->priTxBuffer.GetWSlice();
	auto output = LinkFrame::FormatConfirmedUserData(dest, config.IsMaster, FCB, config.RemoteAddr, config.LocalAddr, tpdu, tpdu.Size(), &logger);
	FORMAT_HEX_BLOCK(logger, flags::LINK_TX_HEX, output, 10, 18);
	return output;
}

RSlice LinkLayer::FormatPrimaryBufferWithUnconfirmed(const openpal::RSlice& tpdu)
{
	auto dest = this->priTxBuffer.GetWSlice();
	auto output = LinkFrame::FormatUnconfirmedUserData(dest, config.IsMaster, config.RemoteAddr, config.LocalAddr, tpdu, tpdu.Size(), &logger);
	FORMAT_HEX_BLOCK(logger, flags::LINK_TX_HEX, output, 10, 18);
	return output;
}

void LinkLayer::QueueTransmit(const RSlice& buffer, bool primary)
{
	if (txMode == LinkTransmitMode::Idle)
	{
		txMode = primary ? LinkTransmitMode::Primary : LinkTransmitMode::Secondary;
		pRouter->BeginTransmit(buffer, *this);
	}
	else
	{
		if (primary)
		{
			pendingPriTx.Set(buffer);
		}
		else
		{
			pendingSecTx.Set(buffer);
		}
	}
}

void LinkLayer::QueueAck()
{
	auto dest = secTxBuffer.GetWSlice();
	auto buffer = LinkFrame::FormatAck(dest, config.IsMaster, false, config.RemoteAddr, config.LocalAddr, &logger);
	FORMAT_HEX_BLOCK(logger, flags::LINK_TX_HEX, buffer, 10, 18);
	this->QueueTransmit(buffer, false);
}

void LinkLayer::QueueLinkStatus()
{
	auto dest = secTxBuffer.GetWSlice();
	auto buffer = LinkFrame::FormatLinkStatus(dest, config.IsMaster, false, config.RemoteAddr, config.LocalAddr, &logger);
	FORMAT_HEX_BLOCK(logger, flags::LINK_TX_HEX, buffer, 10, 18);
	this->QueueTransmit(buffer, false);
}

void LinkLayer::QueueResetLinks()
{
	auto dest = priTxBuffer.GetWSlice();
	auto buffer = LinkFrame::FormatResetLinkStates(dest, config.IsMaster, config.RemoteAddr, config.LocalAddr, &logger);
	FORMAT_HEX_BLOCK(logger, flags::LINK_TX_HEX, buffer, 10, 18);
	this->QueueTransmit(buffer, true);
}

void LinkLayer::QueueRequestLinkStatus()
{
	auto dest = priTxBuffer.GetWSlice();
	auto buffer = LinkFrame::FormatRequestLinkStatus(dest, config.IsMaster, config.RemoteAddr, config.LocalAddr, &logger);
	FORMAT_HEX_BLOCK(logger, flags::LINK_TX_HEX, buffer, 10, 18);
	this->QueueTransmit(buffer, true);
}

void LinkLayer::ResetRetry()
{
	this->numRetryRemaining = config.NumRetry;
}

bool LinkLayer::Retry()
{
	if (numRetryRemaining > 0)
	{
		--numRetryRemaining;
		return true;
	}
	else
	{
		return false;
	}
}

void LinkLayer::PushDataUp(const openpal::RSlice& data)
{
	if (pUpperLayer)
	{
		pUpperLayer->OnReceive(data);
	}
}

void LinkLayer::CompleteSendOperation(bool success)
{
	this->pSegments = nullptr;

	if (pUpperLayer)
	{
		auto callback = [this, success]()
		{
			this->pUpperLayer->OnSendResult(success);
		};

		pExecutor->PostLambda(callback);
	}
}

void LinkLayer::TryStartTransmission()
{
	if (this->keepAliveTimeout)
	{
		this->pPriState = &pPriState->TrySendRequestLinkStatus(*this);
	}

	if (this->pSegments)
	{
		this->pPriState = (this->config.UseConfirms) ? &pPriState->TrySendConfirmed(*this, *pSegments) : &pPriState->TrySendUnconfirmed(*this, *pSegments);
	}
}

void LinkLayer::OnKeepAliveTimeout()
{
	auto now = this->pExecutor->GetTime();

	auto elapsed = now.milliseconds - this->lastMessageTimestamp.milliseconds;

	if (elapsed >= this->config.KeepAliveTimeout.GetMilliseconds())
	{
		this->lastMessageTimestamp = now;
		this->keepAliveTimeout = true;
	}

	// No matter what, reschedule the timer based on last message timestamp
	MonotonicTimestamp expiration(this->lastMessageTimestamp.milliseconds + config.KeepAliveTimeout);
	this->StartKeepAliveTimer(expiration);

	this->TryStartTransmission();
}

void LinkLayer::OnResponseTimeout()
{
	this->pPriState = &(this->pPriState->OnTimeout(*this));

	this->TryStartTransmission();
}

void LinkLayer::StartResponseTimer()
{
	rspTimeoutTimer.Start(
	    TimeDuration(config.Timeout),
	    [this]()
	{
		this->OnResponseTimeout();
	}
	);
}

void LinkLayer::StartKeepAliveTimer(const MonotonicTimestamp& expiration)
{
	this->keepAliveTimer.Start(expiration, [this]()
	{
		this->OnKeepAliveTimeout();
	});
}

void LinkLayer::CancelTimer()
{
	rspTimeoutTimer.Cancel();
}

void LinkLayer::FailKeepAlive(bool timeout)
{
	if (timeout)
	{
		this->pListener->OnKeepAliveFailure();
	}
}

void LinkLayer::CompleteKeepAlive()
{
	this->pListener->OnKeepAliveSuccess();
}

bool LinkLayer::OnFrame(const LinkHeaderFields& header, const openpal::RSlice& userdata)
{
	if (!isOnline)
	{
		SIMPLE_LOG_BLOCK(logger, flags::ERR, "Layer is not online");
		return false;
	}

	if (!this->Validate(header.isFromMaster, header.src, header.dest))
	{
		return false;
	}

	// reset the keep-alive timestamp
	this->lastMessageTimestamp = this->pExecutor->GetTime();

	switch (header.func)
	{
	case(LinkFunction::SEC_ACK) :
		pPriState = &pPriState->OnAck(*this, header.fcvdfc);
		break;
	case(LinkFunction::SEC_NACK) :
		pPriState = &pPriState->OnNack(*this, header.fcvdfc);
		break;
	case(LinkFunction::SEC_LINK_STATUS) :
		pPriState = &pPriState->OnLinkStatus(*this, header.fcvdfc);
		break;
	case(LinkFunction::SEC_NOT_SUPPORTED) :
		pPriState = &pPriState->OnNotSupported(*this, header.fcvdfc);
		break;
	case(LinkFunction::PRI_TEST_LINK_STATES) :
		pSecState = &pSecState->OnTestLinkStatus(*this, header.fcb);
		break;
	case(LinkFunction::PRI_RESET_LINK_STATES) :
		pSecState = &pSecState->OnResetLinkStates(*this);
		break;
	case(LinkFunction::PRI_REQUEST_LINK_STATUS) :
		pSecState = &pSecState->OnRequestLinkStatus(*this);
		break;
	case(LinkFunction::PRI_CONFIRMED_USER_DATA) :
		pSecState = &pSecState->OnConfirmedUserData(*this, header.fcb, userdata);
		break;
	case(LinkFunction::PRI_UNCONFIRMED_USER_DATA) :
		this->PushDataUp(userdata);
		break;
	default:
		return false;;
	}

	TryStartTransmission();

	return true;
}

bool LinkLayer::Validate(bool isMaster, uint16_t src, uint16_t dest)
{
	if (isMaster == config.IsMaster)
	{
		SIMPLE_LOG_BLOCK_WITH_CODE(logger, flags::WARN, DLERR_WRONG_MASTER_BIT,
		                           (isMaster ? "Master frame received for master" : "Outstation frame received for outstation"));

		return false;
	}

	if (dest != config.LocalAddr)
	{
		SIMPLE_LOG_BLOCK_WITH_CODE(logger, flags::WARN, DLERR_UNKNOWN_DESTINATION, "Frame for unknown destintation");
		return false;
	}

	if (src != config.RemoteAddr)
	{
		SIMPLE_LOG_BLOCK_WITH_CODE(logger, flags::WARN, DLERR_UNKNOWN_SOURCE, "Frame from unknwon source");
		return false;
	}

	return true;
}


bool LinkLayer::TryPendingTx(openpal::Settable<RSlice>& pending, bool primary)
{
	if (this->txMode == LinkTransmitMode::Idle && pending.IsSet())
	{
		this->pRouter->BeginTransmit(pending.Get(), *this);
		pending.Clear();
		this->txMode = primary ? LinkTransmitMode::Primary : LinkTransmitMode::Secondary;
		return true;
	}

	return false;
}

void LinkLayer::SetRouter(ILinkTx& pRouter)
{
	this->pRouter = &pRouter;
}

}

