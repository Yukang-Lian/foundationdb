/*
 * GcpTokenProvider.cpp
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

#include "fdbclient/GcpTokenProvider.h"

#ifdef WITH_GCP_BACKUP

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <google/cloud/credentials.h>
#include <google/cloud/oauth2/access_token_generator.h>
#include <google/cloud/options.h>

#include "fdbclient/SystemCaBundle.h"
#include "flow/Error.h"

namespace {

// Same scopes as the Doris object storage client: tokens used against Cloud Storage are limited to storage, and
// when impersonating, the source credential needs cloud-platform to call the IAM Credentials API.
const char* STORAGE_SCOPE = "https://www.googleapis.com/auth/devstorage.read_write";
const char* CLOUD_PLATFORM_SCOPE = "https://www.googleapis.com/auth/cloud-platform";

std::shared_ptr<google::cloud::Credentials> makeCredentials(const GcpCredentialConfig& config) {
	google::cloud::Options options;
	std::string caBundle = systemCaBundle();
	if (!caBundle.empty()) {
		options.set<google::cloud::CARootsFilePathOption>(caBundle);
	}
	bool impersonate = !config.impersonationServiceAccount.empty();
	options.set<google::cloud::ScopesOption>({ impersonate ? CLOUD_PLATFORM_SCOPE : STORAGE_SCOPE });

	std::shared_ptr<google::cloud::Credentials> credentials;
	switch (config.providerType) {
	case GcpCredentialProviderType::DEFAULT:
		credentials = google::cloud::MakeGoogleDefaultCredentials(options);
		break;
	case GcpCredentialProviderType::COMPUTE_ENGINE:
		credentials = google::cloud::MakeComputeEngineCredentials(options);
		break;
	}
	if (impersonate) {
		google::cloud::Options impersonationOptions = options;
		impersonationOptions.set<google::cloud::ScopesOption>({ STORAGE_SCOPE });
		credentials = google::cloud::MakeImpersonateServiceAccountCredentials(
		    std::move(credentials), config.impersonationServiceAccount, std::move(impersonationOptions));
	}
	return credentials;
}

// One generator per distinct configuration, shared by every endpoint of the process, so the SDK's token cache is
// reused and the IAM Credentials API is not called more often than necessary.
std::shared_ptr<google::cloud::oauth2::AccessTokenGenerator> tokenGenerator(const GcpCredentialConfig& config) {
	static std::mutex mutex;
	static std::map<std::string, std::shared_ptr<google::cloud::oauth2::AccessTokenGenerator>> generators;
	std::lock_guard<std::mutex> lock(mutex);
	std::string key =
	    std::string(gcpCredentialProviderTypeName(config.providerType)) + "|" + config.impersonationServiceAccount;
	std::shared_ptr<google::cloud::oauth2::AccessTokenGenerator>& generator = generators[key];
	if (!generator) {
		std::shared_ptr<google::cloud::Credentials> credentials = makeCredentials(config);
		generator = google::cloud::oauth2::MakeAccessTokenGenerator(*credentials);
	}
	return generator;
}

std::string describe(const GcpCredentialConfig& config) {
	std::string s = gcpCredentialProviderTypeName(config.providerType);
	if (!config.impersonationServiceAccount.empty()) {
		s += " impersonating " + config.impersonationServiceAccount;
	}
	return s;
}

} // namespace

GcpAccessToken fetchGcpStorageToken(const GcpCredentialConfig& config) {
	try {
		google::cloud::StatusOr<google::cloud::AccessToken> token = tokenGenerator(config)->GetToken();
		if (!token) {
			fprintf(stderr,
			        "Google Cloud credentials (%s) failed: %s\n",
			        describe(config).c_str(),
			        token.status().message().c_str());
			throw backup_auth_missing();
		}
		double expiresIn = std::chrono::duration<double>(token->expiration - std::chrono::system_clock::now()).count();
		if (token->token.empty() || expiresIn <= 0) {
			fprintf(stderr,
			        "Google Cloud credentials (%s) returned an unusable token (expires in %.0f seconds)\n",
			        describe(config).c_str(),
			        expiresIn);
			throw backup_auth_missing();
		}
		return GcpAccessToken{ token->token, expiresIn };
	} catch (Error&) {
		throw;
	} catch (std::exception& e) {
		fprintf(stderr, "Google Cloud credentials (%s) failed: %s\n", describe(config).c_str(), e.what());
		throw backup_auth_missing();
	}
}

#endif // WITH_GCP_BACKUP
