/*
 * AzureTokenProvider.cpp
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

#include "fdbclient/AzureTokenProvider.h"

#ifdef BUILD_AZURE_BACKUP

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <azure/core/context.hpp>
#include <azure/core/credentials/credentials.hpp>
#include <azure/core/http/curl_transport.hpp>
#include <azure/identity/default_azure_credential.hpp>
#include <azure/identity/managed_identity_credential.hpp>
#include <azure/identity/workload_identity_credential.hpp>

#include "flow/Error.h"
#include "flow/Platform.h"
#include "flow/UnitTest.h"

namespace {

// Azure Storage data-plane access always uses this scope.
const char* const STORAGE_SCOPE = "https://storage.azure.com/.default";

std::string getEnvOrEmpty(const char* name) {
	const char* v = std::getenv(name);
	return v ? v : "";
}

const char* modeName(AzureAuthMode mode) {
	switch (mode) {
	case AzureAuthMode::SHARED_KEY:
		return "shared_key";
	case AzureAuthMode::MANAGED_IDENTITY:
		return "managed_identity";
	case AzureAuthMode::WORKLOAD_IDENTITY:
		return "workload_identity";
	case AzureAuthMode::DEFAULT:
		return "default";
	}
	return "unknown";
}

// The libcurl linked into the binary carries the CA bundle path of the build machine, so give
// the token endpoints (https://login.microsoftonline.com/ and friends) the CA bundle of the
// machine we actually run on.  Same locations as the blobstore TLS client; an empty result
// leaves libcurl's own default in place.
std::string systemCaBundle() {
	static const char* bundles[] = { "/etc/ssl/certs/ca-certificates.crt",
		                             "/etc/pki/tls/certs/ca-bundle.crt",
		                             "/etc/ssl/ca-bundle.pem",
		                             "/etc/pki/tls/cacert.pem",
		                             "/etc/ssl/cert.pem" };
	for (const char* bundle : bundles) {
		if (fileExists(bundle)) {
			return bundle;
		}
	}
	return "";
}

Azure::Core::Credentials::TokenCredentialOptions credentialOptions() {
	Azure::Core::Credentials::TokenCredentialOptions options;
	Azure::Core::Http::CurlTransportOptions curlOptions;
	curlOptions.CAInfo = systemCaBundle();
	options.Transport.Transport = std::make_shared<Azure::Core::Http::CurlTransport>(curlOptions);
	return options;
}

// One credential object per mode for the lifetime of the process: the SDK credentials cache
// their tokens and refresh them internally, which a fresh credential per call would defeat.
std::shared_ptr<Azure::Core::Credentials::TokenCredential> credentialFor(AzureAuthMode mode) {
	static std::mutex mutex;
	static std::map<AzureAuthMode, std::shared_ptr<Azure::Core::Credentials::TokenCredential>> credentials;
	std::lock_guard<std::mutex> lock(mutex);
	auto existing = credentials.find(mode);
	if (existing != credentials.end()) {
		return existing->second;
	}
	std::shared_ptr<Azure::Core::Credentials::TokenCredential> credential;
	switch (mode) {
	case AzureAuthMode::MANAGED_IDENTITY: {
		// An explicitly configured client id selects a user-assigned managed identity; without
		// it the system-assigned identity is used.
		Azure::Identity::ManagedIdentityCredentialOptions options;
		static_cast<Azure::Core::Credentials::TokenCredentialOptions&>(options) = credentialOptions();
		std::string clientId = getEnvOrEmpty("FDB_AZURE_CLIENT_ID");
		if (!clientId.empty()) {
			options.IdentityId = Azure::Identity::ManagedIdentityId::FromUserAssignedClientId(clientId);
		}
		credential = std::make_shared<Azure::Identity::ManagedIdentityCredential>(options);
		break;
	}
	case AzureAuthMode::WORKLOAD_IDENTITY: {
		Azure::Identity::WorkloadIdentityCredentialOptions options;
		static_cast<Azure::Core::Credentials::TokenCredentialOptions&>(options) = credentialOptions();
		credential = std::make_shared<Azure::Identity::WorkloadIdentityCredential>(options);
		break;
	}
	case AzureAuthMode::DEFAULT:
		credential = std::make_shared<Azure::Identity::DefaultAzureCredential>(credentialOptions());
		break;
	default:
		throw backup_auth_missing();
	}
	credentials[mode] = credential;
	return credential;
}

} // namespace

bool parseAzureAuthMode(const std::string& name, AzureAuthMode& mode) {
	if (name.empty() || name == "shared_key") {
		mode = AzureAuthMode::SHARED_KEY;
	} else if (name == "managed_identity") {
		mode = AzureAuthMode::MANAGED_IDENTITY;
	} else if (name == "workload_identity") {
		mode = AzureAuthMode::WORKLOAD_IDENTITY;
	} else if (name == "default") {
		mode = AzureAuthMode::DEFAULT;
	} else {
		return false;
	}
	return true;
}

AzureAuthMode azureAuthModeFromEnvironment() {
	std::string name = getEnvOrEmpty("FDB_AZURE_AUTH_MODE");
	AzureAuthMode mode;
	if (parseAzureAuthMode(name, mode)) {
		return mode;
	}
	fprintf(stderr,
	        "Invalid FDB_AZURE_AUTH_MODE '%s', expected shared_key, managed_identity, workload_identity or default\n",
	        name.c_str());
	throw backup_invalid_url();
}

AzureAccessToken fetchAzureStorageToken(AzureAuthMode mode) {
	if (mode == AzureAuthMode::SHARED_KEY) {
		throw backup_auth_missing();
	}
	try {
		std::shared_ptr<Azure::Core::Credentials::TokenCredential> credential = credentialFor(mode);
		Azure::Core::Credentials::TokenRequestContext request;
		request.Scopes.push_back(STORAGE_SCOPE);
		Azure::Core::Credentials::AccessToken token = credential->GetToken(request, Azure::Core::Context());
		double expiresIn = std::chrono::duration<double>(
		                       static_cast<std::chrono::system_clock::time_point>(token.ExpiresOn) -
		                       std::chrono::system_clock::now())
		                       .count();
		if (token.Token.empty() || expiresIn <= 0) {
			fprintf(stderr,
			        "Azure %s credential returned an unusable token (expires in %.0f seconds)\n",
			        modeName(mode),
			        expiresIn);
			throw backup_auth_missing();
		}
		return AzureAccessToken{ token.Token, expiresIn };
	} catch (Error&) {
		throw;
	} catch (std::exception& e) {
		// Azure::Core::Credentials::AuthenticationException and transport failures
		fprintf(stderr, "Azure %s authentication failed: %s\n", modeName(mode), e.what());
		throw backup_auth_missing();
	}
}

TEST_CASE("/backup/containers/azure/auth_mode") {
	AzureAuthMode mode;
	ASSERT(parseAzureAuthMode("", mode) && mode == AzureAuthMode::SHARED_KEY);
	ASSERT(parseAzureAuthMode("shared_key", mode) && mode == AzureAuthMode::SHARED_KEY);
	ASSERT(parseAzureAuthMode("managed_identity", mode) && mode == AzureAuthMode::MANAGED_IDENTITY);
	ASSERT(parseAzureAuthMode("workload_identity", mode) && mode == AzureAuthMode::WORKLOAD_IDENTITY);
	ASSERT(parseAzureAuthMode("default", mode) && mode == AzureAuthMode::DEFAULT);
	ASSERT(!parseAzureAuthMode("Default", mode));
	ASSERT(!parseAzureAuthMode("imds", mode));
	return Void();
}

#endif // BUILD_AZURE_BACKUP
