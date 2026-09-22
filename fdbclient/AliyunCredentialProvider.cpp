/*
 * AliyunCredentialProvider.cpp
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

#include "fdbclient/AliyunCredentialProvider.h"

#ifdef WITH_ALIYUN_BACKUP

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <alibabacloud/credentials/Client.hpp>
#include <alibabacloud/credentials/Model.hpp>
#include <alibabacloud/credentials/provider/DefaultProvider.hpp>
#include <alibabacloud/credentials/provider/EcsRamRoleProvider.hpp>
#include <alibabacloud/credentials/provider/EnvironmentVariableProvider.hpp>
#include <alibabacloud/credentials/provider/OIDCRoleArnProvider.hpp>

#include "flow/Error.h"

namespace {

using AlibabaCloud::Credentials::Client;
using AlibabaCloud::Credentials::Provider;

bool ecsMetadataDisabled() {
	const char* v = getenv("ALIBABA_CLOUD_ECS_METADATA_DISABLED");
	return v != nullptr && (strcmp(v, "true") == 0 || strcmp(v, "True") == 0 || strcmp(v, "TRUE") == 0);
}

std::shared_ptr<Client> makeClient(AliyunCredentialProviderType type) {
	using namespace AlibabaCloud::Credentials;
	std::shared_ptr<Provider> provider;
	std::shared_ptr<Models::Config> sdkConfig = std::make_shared<Models::Config>();
	switch (type) {
	case AliyunCredentialProviderType::ENV:
		provider = std::make_shared<EnvironmentVariableProvider>();
		break;
	case AliyunCredentialProviderType::ECS_RAM_ROLE:
		// the role name is discovered from the metadata service (or ALIBABA_CLOUD_ECS_METADATA)
		sdkConfig->setType("ecs_ram_role");
		provider = std::make_shared<EcsRamRoleProvider>(sdkConfig);
		break;
	case AliyunCredentialProviderType::OIDC_ROLE_ARN:
		// ALIBABA_CLOUD_ROLE_ARN, ALIBABA_CLOUD_OIDC_PROVIDER_ARN, ALIBABA_CLOUD_OIDC_TOKEN_FILE from the pod
		sdkConfig->setType("oidc_role_arn");
		provider = std::make_shared<OIDCRoleArnProvider>(sdkConfig);
		break;
	case AliyunCredentialProviderType::DEFAULT:
	default:
		provider = std::make_shared<DefaultProvider>();
		break;
	}
	return std::make_shared<Client>(provider);
}

// One SDK client per credential source, shared by every endpoint of the process. The SDK caches the temporary
// credentials it obtains and refreshes them before they expire. Construction can throw (the SDK rejects some
// environments up front), which the caller reports as that source failing.
std::shared_ptr<Client> clientFor(AliyunCredentialProviderType type) {
	static std::mutex mutex;
	static std::map<AliyunCredentialProviderType, std::shared_ptr<Client>> clients;
	std::lock_guard<std::mutex> lock(mutex);
	std::shared_ptr<Client>& client = clients[type];
	if (!client) {
		client = makeClient(type);
	}
	return client;
}

bool tryGet(AliyunCredentialProviderType type, AliyunCredentials* result, std::string* error) {
	try {
		AlibabaCloud::Credentials::Models::CredentialModel credential = clientFor(type)->getCredential();
		result->key = credential.getAccessKeyId();
		result->secret = credential.getAccessKeySecret();
		result->token = credential.getSecurityToken();
		if (result->key.empty() || result->secret.empty()) {
			*error = "no access key";
			return false;
		}
		return true;
	} catch (std::exception& e) {
		*error = e.what();
		return false;
	}
}

// The source that last served DEFAULT, so later refreshes do not walk the whole chain again.
std::mutex resolvedDefaultMutex;
bool resolvedDefaultKnown = false;
AliyunCredentialProviderType resolvedDefault = AliyunCredentialProviderType::DEFAULT;

} // namespace

AliyunCredentials fetchAliyunCredentials(const AliyunCredentialConfig& config) {
	// DEFAULT is the SDK's default chain, which only consults the ECS instance role when
	// ALIBABA_CLOUD_ECS_METADATA names it. Like the AWS and Google defaults, fdbbackup's DEFAULT then falls
	// back to the instance role with the role name discovered from the metadata service.
	std::vector<AliyunCredentialProviderType> order;
	if (config.providerType == AliyunCredentialProviderType::DEFAULT) {
		{
			std::lock_guard<std::mutex> lock(resolvedDefaultMutex);
			if (resolvedDefaultKnown) {
				order.push_back(resolvedDefault);
			}
		}
		order.push_back(AliyunCredentialProviderType::DEFAULT);
		if (!ecsMetadataDisabled()) {
			order.push_back(AliyunCredentialProviderType::ECS_RAM_ROLE);
		}
	} else {
		order.push_back(config.providerType);
	}

	std::string errors;
	for (AliyunCredentialProviderType type : order) {
		AliyunCredentials result;
		std::string error;
		if (tryGet(type, &result, &error)) {
			result.source = type;
			if (config.providerType == AliyunCredentialProviderType::DEFAULT) {
				std::lock_guard<std::mutex> lock(resolvedDefaultMutex);
				resolvedDefaultKnown = true;
				resolvedDefault = type;
			}
			return result;
		}
		errors += std::string(aliyunCredentialProviderTypeName(type)) + ": " + error + "; ";
	}
	fprintf(stderr,
	        "Alibaba Cloud credentials (%s) failed: %s\n",
	        aliyunCredentialProviderTypeName(config.providerType),
	        errors.c_str());
	throw backup_auth_missing();
}

#endif // WITH_ALIYUN_BACKUP
