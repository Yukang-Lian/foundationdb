/*
 * GcpTokenProvider.h
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

#ifndef FDBCLIENT_GCP_TOKEN_PROVIDER_H
#define FDBCLIENT_GCP_TOKEN_PROVIDER_H
#pragma once

#include <cctype>
#include <string>

// Where the Google Cloud source credential comes from when a blobstore URL sets gcp_auth. The values match the
// gcs.credential_provider_type setting of the Doris object storage client:
//   DEFAULT        the SDK's application default credentials lookup: GOOGLE_APPLICATION_CREDENTIALS, then the
//                  gcloud ADC file, then the GCE/GKE metadata server
//   COMPUTE_ENGINE only the GCE/GKE metadata server, so files and developer logins on the host cannot change
//                  the identity
enum class GcpCredentialProviderType { DEFAULT, COMPUTE_ENGINE };

struct GcpCredentialConfig {
	GcpCredentialProviderType providerType = GcpCredentialProviderType::DEFAULT;
	// When set, the source credential is exchanged for an access token of this service account through the IAM
	// Service Account Credentials API, so Cloud Storage sees that account instead of the source identity.
	std::string impersonationServiceAccount;
};

inline const char* gcpCredentialProviderTypeName(GcpCredentialProviderType type) {
	return type == GcpCredentialProviderType::COMPUTE_ENGINE ? "COMPUTE_ENGINE" : "DEFAULT";
}

// Accepts DEFAULT and COMPUTE_ENGINE in any letter case. Returns false for anything else.
inline bool parseGcpCredentialProviderType(const std::string& name, GcpCredentialProviderType* type) {
	std::string upper;
	for (char c : name) {
		upper.push_back(toupper(static_cast<unsigned char>(c)));
	}
	if (upper == "DEFAULT") {
		*type = GcpCredentialProviderType::DEFAULT;
		return true;
	}
	if (upper == "COMPUTE_ENGINE") {
		*type = GcpCredentialProviderType::COMPUTE_ENGINE;
		return true;
	}
	return false;
}

// A service account email, e.g. name@project.iam.gserviceaccount.com (the same domains Doris accepts).
inline bool isValidGcpServiceAccountEmail(const std::string& email) {
	size_t at = email.find('@');
	if (at == 0 || at == std::string::npos || email.find('@', at + 1) != std::string::npos) {
		return false;
	}
	for (char c : email) {
		if (isspace(static_cast<unsigned char>(c))) {
			return false;
		}
	}
	std::string domain = email.substr(at + 1);
	if (domain == "developer.gserviceaccount.com" || domain == "appspot.gserviceaccount.com") {
		return true;
	}
	const std::string suffix = ".iam.gserviceaccount.com";
	return domain.size() > suffix.size() && domain.compare(domain.size() - suffix.size(), suffix.size(), suffix) == 0;
}

#ifdef WITH_GCP_BACKUP
struct GcpAccessToken {
	std::string token;
	double expiresIn = 0;
};

// Fetches an OAuth2 access token for Cloud Storage with the Google Cloud SDK. Blocks, so callers run it on a
// separate thread. Throws backup_auth_missing when no usable credentials are found.
GcpAccessToken fetchGcpStorageToken(const GcpCredentialConfig& config);
#endif

#endif
