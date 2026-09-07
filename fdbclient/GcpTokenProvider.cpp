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
#include <memory>
#include <mutex>
#include <string>

#include <google/cloud/credentials.h>
#include <google/cloud/oauth2/access_token_generator.h>
#include <google/cloud/options.h>

#include "flow/Error.h"
#include "flow/Platform.h"

namespace {

// The libcurl linked into the binary carries the CA bundle path of the build machine, so give
// the token endpoints the CA bundle of the machine we actually run on.  Same locations as the
// blobstore TLS client; an empty result leaves the SDK's default in place.
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

// One generator for the lifetime of the process: the SDK credentials cache their tokens and
// refresh them internally, which a fresh generator per call would defeat.
std::shared_ptr<google::cloud::oauth2::AccessTokenGenerator> tokenGenerator() {
	static std::mutex mutex;
	static std::shared_ptr<google::cloud::oauth2::AccessTokenGenerator> generator;
	std::lock_guard<std::mutex> lock(mutex);
	if (!generator) {
		google::cloud::Options options;
		std::string caBundle = systemCaBundle();
		if (!caBundle.empty()) {
			options.set<google::cloud::CARootsFilePathOption>(caBundle);
		}
		std::shared_ptr<google::cloud::Credentials> credentials = google::cloud::MakeGoogleDefaultCredentials(options);
		generator = google::cloud::oauth2::MakeAccessTokenGenerator(*credentials);
	}
	return generator;
}

} // namespace

GcpAccessToken fetchGcpStorageToken() {
	try {
		google::cloud::StatusOr<google::cloud::AccessToken> token = tokenGenerator()->GetToken();
		if (!token) {
			fprintf(stderr,
			        "Google Cloud application default credentials failed: %s\n",
			        token.status().message().c_str());
			throw backup_auth_missing();
		}
		double expiresIn =
		    std::chrono::duration<double>(token->expiration - std::chrono::system_clock::now()).count();
		if (token->token.empty() || expiresIn <= 0) {
			fprintf(stderr,
			        "Google Cloud credentials returned an unusable token (expires in %.0f seconds)\n",
			        expiresIn);
			throw backup_auth_missing();
		}
		return GcpAccessToken{ token->token, expiresIn };
	} catch (Error&) {
		throw;
	} catch (std::exception& e) {
		fprintf(stderr, "Google Cloud credentials failed: %s\n", e.what());
		throw backup_auth_missing();
	}
}

#endif // WITH_GCP_BACKUP
