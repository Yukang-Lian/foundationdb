/*
 * AliyunCredentialProvider.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FDBCLIENT_ALIYUN_CREDENTIAL_PROVIDER_H
#define FDBCLIENT_ALIYUN_CREDENTIAL_PROVIDER_H
#pragma once

#include <cctype>
#include <string>

// Where the Alibaba Cloud credentials of aliyun_auth come from. The SDK's default chain covers every deployment
// form; the explicit types pin one source so that other credentials present on the host cannot take over.
//   DEFAULT       the Alibaba Cloud Credentials SDK default chain: ALIBABA_CLOUD_ACCESS_KEY_ID environment
//                 variables, RRSA (OIDC) variables, CLI and profile files, the ECS instance RAM role, then
//                 ALIBABA_CLOUD_CREDENTIALS_URI
//   ENV           ALIBABA_CLOUD_ACCESS_KEY_ID / ALIBABA_CLOUD_ACCESS_KEY_SECRET (/ ALIBABA_CLOUD_SECURITY_TOKEN)
//   ECS_RAM_ROLE  the RAM role attached to the ECS instance, through the instance metadata service
//   OIDC_ROLE_ARN ACK RRSA: ALIBABA_CLOUD_ROLE_ARN, ALIBABA_CLOUD_OIDC_PROVIDER_ARN and
//                 ALIBABA_CLOUD_OIDC_TOKEN_FILE injected into the pod, exchanged through STS AssumeRoleWithOIDC
enum class AliyunCredentialProviderType { DEFAULT, ENV, ECS_RAM_ROLE, OIDC_ROLE_ARN };

struct AliyunCredentialConfig {
	AliyunCredentialProviderType providerType = AliyunCredentialProviderType::DEFAULT;
};

inline const char* aliyunCredentialProviderTypeName(AliyunCredentialProviderType type) {
	switch (type) {
	case AliyunCredentialProviderType::ENV:
		return "ENV";
	case AliyunCredentialProviderType::ECS_RAM_ROLE:
		return "ECS_RAM_ROLE";
	case AliyunCredentialProviderType::OIDC_ROLE_ARN:
		return "OIDC_ROLE_ARN";
	case AliyunCredentialProviderType::DEFAULT:
	default:
		return "DEFAULT";
	}
}

// Accepts the names above in any letter case. Returns false for anything else.
inline bool parseAliyunCredentialProviderType(const std::string& name, AliyunCredentialProviderType* type) {
	std::string upper;
	for (char c : name) {
		upper.push_back(toupper(static_cast<unsigned char>(c)));
	}
	static const AliyunCredentialProviderType all[] = { AliyunCredentialProviderType::DEFAULT,
		                                                AliyunCredentialProviderType::ENV,
		                                                AliyunCredentialProviderType::ECS_RAM_ROLE,
		                                                AliyunCredentialProviderType::OIDC_ROLE_ARN };
	for (AliyunCredentialProviderType candidate : all) {
		if (upper == aliyunCredentialProviderTypeName(candidate)) {
			*type = candidate;
			return true;
		}
	}
	return false;
}

struct AliyunCredentials {
	std::string key;
	std::string secret;
	std::string token;
	// the source that served them; for DEFAULT this tells which step of the chain answered
	AliyunCredentialProviderType source = AliyunCredentialProviderType::DEFAULT;
};

#ifdef WITH_ALIYUN_BACKUP
// Resolves credentials with the Alibaba Cloud Credentials SDK. Blocks on network calls (metadata service, STS),
// so callers run it off the network thread. Clients are cached per configuration so the SDK refreshes temporary
// credentials itself. Throws backup_auth_missing when no credentials can be obtained.
AliyunCredentials fetchAliyunCredentials(const AliyunCredentialConfig& config);
#endif

#endif
