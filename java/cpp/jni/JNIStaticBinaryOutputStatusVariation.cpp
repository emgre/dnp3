//
//  _   _         ______    _ _ _   _             _ _ _
// | \ | |       |  ____|  | (_) | (_)           | | | |
// |  \| | ___   | |__   __| |_| |_ _ _ __   __ _| | | |
// | . ` |/ _ \  |  __| / _` | | __| | '_ \ / _` | | | |
// | |\  | (_) | | |___| (_| | | |_| | | | | (_| |_|_|_|
// |_| \_|\___/  |______\__,_|_|\__|_|_| |_|\__, (_|_|_)
//                                           __/ |
//                                          |___/
// 
// This file is auto-generated. Do not edit manually
// 
// Copyright 2016 Automatak LLC
// 
// Automatak LLC (www.automatak.com) licenses this file
// to you under the the Apache License Version 2.0 (the "License"):
// 
// http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "JNIStaticBinaryOutputStatusVariation.h"

namespace jni
{
    namespace cache
    {
        bool StaticBinaryOutputStatusVariation::init(JNIEnv* env)
        {
            auto clazzTemp = env->FindClass("Lcom/automatak/dnp3/enums/StaticBinaryOutputStatusVariation;");
            this->clazz = (jclass) env->NewGlobalRef(clazzTemp);
            env->DeleteLocalRef(clazzTemp);

            this->toTypeMethod = env->GetMethodID(this->clazz, "toType", "()I");
            if(!this->toTypeMethod) return false;

            this->fromTypeMethod = env->GetStaticMethodID(this->clazz, "fromType", "(I)Lcom/automatak/dnp3/enums/StaticBinaryOutputStatusVariation;");
            if(!this->fromTypeMethod) return false;

            return true;
        }

        void StaticBinaryOutputStatusVariation::cleanup(JNIEnv* env)
        {
            env->DeleteGlobalRef(this->clazz);
        }

        jint StaticBinaryOutputStatusVariation::toType(JNIEnv* env, jobject instance)
        {
            return env->CallIntMethod(instance, this->toTypeMethod);
        }

        LocalRef<jobject> StaticBinaryOutputStatusVariation::fromType(JNIEnv* env, jint arg0)
        {
            return LocalRef<jobject>(env, env->CallStaticObjectMethod(this->clazz, this->fromTypeMethod, arg0));
        }
    }
}
