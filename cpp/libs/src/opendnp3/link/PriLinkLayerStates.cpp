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
#include "PriLinkLayerStates.h"


#include <openpal/logging/LogMacros.h>

#include "opendnp3/ErrorCodes.h"
#include "opendnp3/link/LinkLayer.h"
#include "opendnp3/LogLevels.h"

using namespace openpal;

namespace opendnp3
{

////////////////////////////////////////
// PriStateBase
////////////////////////////////////////

PriStateBase& PriStateBase::OnAck(LinkLayer& ctx, bool rxBuffFull)
{
	SIMPLE_LOG_BLOCK_WITH_CODE(ctx.logger, flags::WARN, DLERR_UNEXPECTED_LPDU, "Frame context not understood");
	return *this;
}

PriStateBase& PriStateBase::OnNack(LinkLayer& ctx, bool rxBuffFull)
{
	SIMPLE_LOG_BLOCK_WITH_CODE(ctx.logger, flags::WARN, DLERR_UNEXPECTED_LPDU, "Frame context not understood");
	return *this;
}

PriStateBase& PriStateBase::OnLinkStatus(LinkLayer& ctx, bool rxBuffFull)
{
	SIMPLE_LOG_BLOCK_WITH_CODE(ctx.logger, flags::WARN, DLERR_UNEXPECTED_LPDU, "Frame context not understood");
	return *this;
}

PriStateBase& PriStateBase::OnNotSupported(LinkLayer& ctx, bool rxBuffFull)
{
	SIMPLE_LOG_BLOCK_WITH_CODE(ctx.logger, flags::WARN, DLERR_UNEXPECTED_LPDU, "Frame context not understood");
	return *this;
}

PriStateBase& PriStateBase::OnTransmitResult(LinkLayer& ctx, bool success)
{
	FORMAT_LOG_BLOCK(ctx.logger, flags::ERR, "Invalid action for state: %s", this->Name());
	return *this;
}

PriStateBase& PriStateBase::OnTimeout(LinkLayer& ctx)
{
	FORMAT_LOG_BLOCK(ctx.logger, flags::ERR, "Invalid action for state: %s", this->Name());
	return *this;
}

PriStateBase& PriStateBase::TrySendConfirmed(LinkLayer& ctx, ITransportSegment&)
{
	return *this;
}

PriStateBase& PriStateBase::TrySendUnconfirmed(LinkLayer& ctx, ITransportSegment&)
{
	return *this;
}

PriStateBase& PriStateBase::TrySendRequestLinkStatus(LinkLayer&)
{
	return *this;
}

////////////////////////////////////////////////////////
//	Class PLLS_SecNotResetIdle
////////////////////////////////////////////////////////

PLLS_Idle PLLS_Idle::instance;

PriStateBase& PLLS_Idle::TrySendUnconfirmed(LinkLayer& ctx, ITransportSegment& segments)
{
	auto first = segments.GetSegment();
	auto output = ctx.FormatPrimaryBufferWithUnconfirmed(first);
	ctx.QueueTransmit(output, true);
	return PLLS_SendUnconfirmedTransmitWait::Instance();
}

PriStateBase& PLLS_Idle::TrySendConfirmed(LinkLayer& ctx, ITransportSegment& segments)
{
	if (ctx.isRemoteReset)
	{
		ctx.ResetRetry();
		auto buffer = ctx.FormatPrimaryBufferWithConfirmed(segments.GetSegment(), ctx.nextWriteFCB);
		ctx.QueueTransmit(buffer, true);
		return PLLS_ConfUserDataTransmitWait::Instance();
	}
	else
	{
		ctx.ResetRetry();
		ctx.QueueResetLinks();
		return PLLS_LinkResetTransmitWait::Instance();
	}
}

PriStateBase& PLLS_Idle::TrySendRequestLinkStatus(LinkLayer& ctx)
{
	ctx.keepAliveTimeout = false;
	ctx.QueueRequestLinkStatus();
	ctx.pListener->OnKeepAliveInitiated();
	return PLLS_RequestLinkStatusTransmitWait::Instance();
}

////////////////////////////////////////////////////////
//	Class SendUnconfirmedTransmitWait
////////////////////////////////////////////////////////

PLLS_SendUnconfirmedTransmitWait PLLS_SendUnconfirmedTransmitWait::instance;

PriStateBase& PLLS_SendUnconfirmedTransmitWait::OnTransmitResult(LinkLayer& ctx, bool success)
{
	if (ctx.pSegments->Advance())
	{
		auto output = ctx.FormatPrimaryBufferWithUnconfirmed(ctx.pSegments->GetSegment());
		ctx.QueueTransmit(output, true);
		return *this;
	}
	else // we're done
	{
		ctx.CompleteSendOperation(success);
		return PLLS_Idle::Instance();
	}
}


/////////////////////////////////////////////////////////////////////////////
//  Wait for the link layer to transmit the reset links
/////////////////////////////////////////////////////////////////////////////

PLLS_LinkResetTransmitWait PLLS_LinkResetTransmitWait::instance;

PriStateBase& PLLS_LinkResetTransmitWait::OnTransmitResult(LinkLayer& ctx, bool success)
{
	if (success)
	{
		// now we're waiting for an ACK
		ctx.StartResponseTimer();
		return PLLS_ResetLinkWait::Instance();
	}
	else
	{
		ctx.CompleteSendOperation(success);
		return PLLS_Idle::Instance();
	}
}

/////////////////////////////////////////////////////////////////////////////
//  Wait for the link layer to transmit confirmed user data
/////////////////////////////////////////////////////////////////////////////

PLLS_ConfUserDataTransmitWait PLLS_ConfUserDataTransmitWait::instance;

PriStateBase& PLLS_ConfUserDataTransmitWait::OnTransmitResult(LinkLayer& ctx, bool success)
{
	if (success)
	{
		// now we're waiting on an ACK
		ctx.StartResponseTimer();
		return PLLS_ConfDataWait::Instance();
	}
	else
	{
		ctx.CompleteSendOperation(false);
		return PLLS_Idle::Instance();
	}
}

/////////////////////////////////////////////////////////////////////////////
//  Wait for the link layer to transmit the request link status
/////////////////////////////////////////////////////////////////////////////

PLLS_RequestLinkStatusTransmitWait PLLS_RequestLinkStatusTransmitWait::instance;

PriStateBase& PLLS_RequestLinkStatusTransmitWait::OnTransmitResult(LinkLayer& ctx, bool success)
{
	if (success)
	{
		// now we're waiting on a LINK_STATUS
		ctx.StartResponseTimer();
		return PLLS_RequestLinkStatusWait::Instance();
	}
	else
	{
		ctx.FailKeepAlive(false);
		return PLLS_Idle::Instance();
	}
}


////////////////////////////////////////////////////////
//	Class PLLS_ResetLinkWait
////////////////////////////////////////////////////////

PLLS_ResetLinkWait PLLS_ResetLinkWait::instance;

PriStateBase& PLLS_ResetLinkWait::OnAck(LinkLayer& ctx, bool rxBuffFull)
{
	ctx.isRemoteReset = true;
	ctx.ResetWriteFCB();
	ctx.CancelTimer();
	auto buffer = ctx.FormatPrimaryBufferWithConfirmed(ctx.pSegments->GetSegment(), ctx.nextWriteFCB);
	ctx.QueueTransmit(buffer, true);
	ctx.pListener->OnStateChange(opendnp3::LinkStatus::RESET);
	return PLLS_ConfUserDataTransmitWait::Instance();
}

PriStateBase& PLLS_ResetLinkWait::OnTimeout(LinkLayer& ctx)
{
	if(ctx.Retry())
	{
		FORMAT_LOG_BLOCK(ctx.logger, flags::WARN, "Link reset timeout, retrying %i remaining", ctx.numRetryRemaining);
		ctx.QueueResetLinks();
		return PLLS_LinkResetTransmitWait::Instance();
	}
	else
	{
		SIMPLE_LOG_BLOCK(ctx.logger, flags::WARN, "Link reset final timeout, no retries remain");
		ctx.CompleteSendOperation(false);
		return PLLS_Idle::Instance();
	}
}

PriStateBase& PLLS_ResetLinkWait::Failure(LinkLayer& ctx)
{
	ctx.CancelTimer();
	ctx.CompleteSendOperation(false);
	return PLLS_Idle::Instance();
}

////////////////////////////////////////////////////////
//	Class PLLS_ConfDataWait
////////////////////////////////////////////////////////

PLLS_ConfDataWait PLLS_ConfDataWait::instance;

PriStateBase& PLLS_ConfDataWait::OnAck(LinkLayer& ctx, bool rxBuffFull)
{
	ctx.ToggleWriteFCB();
	ctx.CancelTimer();

	if (ctx.pSegments->Advance())
	{
		auto buffer = ctx.FormatPrimaryBufferWithConfirmed(ctx.pSegments->GetSegment(), ctx.nextWriteFCB);
		ctx.QueueTransmit(buffer, true);
		return PLLS_ConfUserDataTransmitWait::Instance();
	}
	else //we're done!
	{
		ctx.CompleteSendOperation(true);
		return PLLS_Idle::Instance();
	}
}

PriStateBase& PLLS_ConfDataWait::OnNack(LinkLayer& ctx, bool rxBuffFull)
{
	ctx.pListener->OnStateChange(opendnp3::LinkStatus::UNRESET);

	if (rxBuffFull)
	{
		return Failure(ctx);
	}
	else
	{
		ctx.ResetRetry();
		ctx.CancelTimer();
		ctx.QueueResetLinks();
		return PLLS_LinkResetTransmitWait::Instance();
	}

}

PriStateBase& PLLS_ConfDataWait::Failure(LinkLayer& ctx)
{
	ctx.CancelTimer();
	ctx.CompleteSendOperation(false);
	return PLLS_Idle::Instance();
}

PriStateBase& PLLS_ConfDataWait::OnTimeout(LinkLayer& ctx)
{
	if (ctx.Retry())
	{
		FORMAT_LOG_BLOCK(ctx.logger, flags::WARN, "confirmed data timeout, retrying %u remaining", ctx.numRetryRemaining);
		auto buffer = ctx.FormatPrimaryBufferWithConfirmed(ctx.pSegments->GetSegment(), ctx.nextWriteFCB);
		ctx.QueueTransmit(buffer, true);
		return PLLS_ConfUserDataTransmitWait::Instance();
	}
	else
	{
		SIMPLE_LOG_BLOCK(ctx.logger, flags::WARN, "Confirmed data final timeout, no retries remain");
		ctx.pListener->OnStateChange(opendnp3::LinkStatus::UNRESET);
		ctx.CompleteSendOperation(false);
		return PLLS_Idle::Instance();
	}
}

////////////////////////////////////////////////////////
//	Class PLLS_RequestLinkStatusWait
////////////////////////////////////////////////////////

PLLS_RequestLinkStatusWait PLLS_RequestLinkStatusWait::instance;

PriStateBase& PLLS_RequestLinkStatusWait::OnNack(LinkLayer& ctx, bool)
{
	ctx.CancelTimer();
	ctx.FailKeepAlive(false);
	return PLLS_Idle::Instance();
}

PriStateBase& PLLS_RequestLinkStatusWait::OnLinkStatus(LinkLayer& ctx, bool)
{
	ctx.CancelTimer();
	ctx.CompleteKeepAlive();
	return PLLS_Idle::Instance();
}

PriStateBase& PLLS_RequestLinkStatusWait::OnNotSupported(LinkLayer& ctx, bool)
{
	ctx.CancelTimer();
	ctx.FailKeepAlive(false);
	return PLLS_Idle::Instance();
}

PriStateBase& PLLS_RequestLinkStatusWait::OnTimeout(LinkLayer& ctx)
{
	SIMPLE_LOG_BLOCK(ctx.logger, flags::WARN, "Link status request - response timeout");
	ctx.FailKeepAlive(true);
	return PLLS_Idle::Instance();
}

} //end namepsace

