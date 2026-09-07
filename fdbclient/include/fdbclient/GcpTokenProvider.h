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

#if (!defined FDBCLIENT_GCP_TOKEN_PROVIDER_H) && (defined WITH_GCP_BACKUP)
#define FDBCLIENT_GCP_TOKEN_PROVIDER_H
#pragma once

#include <string>

struct GcpAccessToken {
	std::string token;
	// Seconds until the token expires, relative to the moment it was fetched.
	double expiresIn = 0;
};

// Fetches an OAuth2 access token for Google Cloud Storage through google-cloud-cpp's Application
// Default Credentials, the Google counterpart of the AWS SDK credential chain used for sdk_auth:
// GOOGLE_APPLICATION_CREDENTIALS (service account key or external account files), gcloud user
// credentials, and the GCE/GKE metadata server (GCE_METADATA_ROOT overrides its address).
// This is a synchronous, blocking call into the SDK: it must run on a background thread, never on
// the flow network thread.  Throws backup_auth_missing() on failure.
GcpAccessToken fetchGcpStorageToken();

#endif
