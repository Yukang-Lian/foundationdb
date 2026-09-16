/*
 * FDBAWSCredentialsProvider.h
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

#ifndef FDB_AWS_CREDENTIALS_PROVIDER_H
#define FDB_AWS_CREDENTIALS_PROVIDER_H
#pragma once

#include <cctype>
#include <string>

// Which AWS SDK credentials provider supplies the base credentials of sdk_auth. The values match the
// s3.credentials_provider_type setting of Doris (its ANONYMOUS is not offered: backups need write access).
//   DEFAULT          the SDK's default chain: environment, profile file, process, web identity, SSO, ECS
//                    container, EC2 instance metadata
//   ENV              AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY (/ AWS_SESSION_TOKEN)
//   SYSTEM_PROPERTIES the profile file (~/.aws/credentials, AWS_PROFILE)
//   WEB_IDENTITY     AWS_ROLE_ARN + AWS_WEB_IDENTITY_TOKEN_FILE (EKS IAM roles for service accounts)
//   CONTAINER        the ECS container credentials endpoint
//   INSTANCE_PROFILE the EC2 instance metadata service
enum class AwsCredentialProviderType { DEFAULT, ENV, SYSTEM_PROPERTIES, WEB_IDENTITY, CONTAINER, INSTANCE_PROFILE };

struct AwsCredentialConfig {
	AwsCredentialProviderType providerType = AwsCredentialProviderType::DEFAULT;
	// When set, the base credentials are exchanged through STS AssumeRole for temporary credentials of this role
	// (with the external ID, if the role's trust policy demands one). The SDK refreshes the session by itself.
	std::string roleArn;
	std::string externalId;
	// Region of the STS endpoint used for AssumeRole: the blobstore's region.
	std::string region;
};

inline const char* awsCredentialProviderTypeName(AwsCredentialProviderType type) {
	switch (type) {
	case AwsCredentialProviderType::ENV:
		return "ENV";
	case AwsCredentialProviderType::SYSTEM_PROPERTIES:
		return "SYSTEM_PROPERTIES";
	case AwsCredentialProviderType::WEB_IDENTITY:
		return "WEB_IDENTITY";
	case AwsCredentialProviderType::CONTAINER:
		return "CONTAINER";
	case AwsCredentialProviderType::INSTANCE_PROFILE:
		return "INSTANCE_PROFILE";
	case AwsCredentialProviderType::DEFAULT:
	default:
		return "DEFAULT";
	}
}

// Accepts the names above in any letter case. Returns false for anything else.
inline bool parseAwsCredentialProviderType(const std::string& name, AwsCredentialProviderType* type) {
	std::string upper;
	for (char c : name) {
		upper.push_back(toupper(static_cast<unsigned char>(c)));
	}
	static const AwsCredentialProviderType all[] = {
		AwsCredentialProviderType::DEFAULT,           AwsCredentialProviderType::ENV,
		AwsCredentialProviderType::SYSTEM_PROPERTIES, AwsCredentialProviderType::WEB_IDENTITY,
		AwsCredentialProviderType::CONTAINER,         AwsCredentialProviderType::INSTANCE_PROFILE
	};
	for (AwsCredentialProviderType candidate : all) {
		if (upper == awsCredentialProviderTypeName(candidate)) {
			*type = candidate;
			return true;
		}
	}
	return false;
}

#ifdef WITH_AWS_BACKUP
#include "aws/core/Aws.h"
#include "aws/core/auth/AWSCredentialsProviderChain.h"

namespace FDBAWSCredentialsProvider {
// Resolves credentials as configured. Blocks on network calls (metadata service, STS), so callers run it off the
// network thread. Providers are cached per configuration, so the SDK reuses and refreshes assumed-role sessions
// instead of calling STS for every connection.
Aws::Auth::AWSCredentials getAwsCredentials(const AwsCredentialConfig& config);
} // namespace FDBAWSCredentialsProvider
#endif

#endif
