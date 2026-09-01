/*
 * AzureTokenProvider.h
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

#if (!defined FDBCLIENT_AZURE_TOKEN_PROVIDER_H) && (defined BUILD_AZURE_BACKUP)
#define FDBCLIENT_AZURE_TOKEN_PROVIDER_H
#pragma once

#include <string>

// How the Azure Blob backup container authenticates against Azure Storage.
//
// SHARED_KEY is the original behavior: the storage account key is taken from the
// AZURE_KEY environment variable and requests are signed with it.
//
// MANAGED_IDENTITY obtains an OAuth bearer token for https://storage.azure.com/ from the
// Azure Instance Metadata Service (IMDS) of the VM/VMSS the process runs on.  The optional
// FDB_AZURE_CLIENT_ID environment variable selects a user-assigned identity.
//
// WORKLOAD_IDENTITY obtains an OAuth bearer token using the federated service account
// token that Azure Workload Identity (e.g. on AKS) projects into the pod.  It uses the
// standard environment variables injected by the workload identity webhook:
// AZURE_CLIENT_ID, AZURE_TENANT_ID, AZURE_FEDERATED_TOKEN_FILE and AZURE_AUTHORITY_HOST.
//
// The mode is selected explicitly with the FDB_AZURE_AUTH_MODE environment variable
// ("shared_key", "managed_identity" or "workload_identity").  There is intentionally no
// implicit credential chain: an unset variable selects SHARED_KEY, the historical behavior.
enum class AzureAuthMode { SHARED_KEY, MANAGED_IDENTITY, WORKLOAD_IDENTITY };

struct AzureAccessToken {
	std::string token;
	// Seconds until the token expires, relative to the moment it was fetched.
	double expiresIn = 0;
};

// Reads FDB_AZURE_AUTH_MODE and returns the selected mode.  Throws backup_invalid_url on an
// unrecognized value.
AzureAuthMode azureAuthModeFromEnvironment();

// Fetches a bearer token for scope https://storage.azure.com/ using the given mode, which
// must not be SHARED_KEY.  This performs a synchronous, blocking network call: it must run
// on a background thread (e.g. the backup container's AsyncTaskThread), never on the flow
// network thread.  Throws backup_auth_missing() on failure.
AzureAccessToken fetchAzureStorageToken(AzureAuthMode mode);

// Parses an OAuth token response body ({"access_token": ..., "expires_in": ...}) from IMDS
// (expires_in as a JSON string) or from the AAD v2 token endpoint (expires_in as a JSON
// number).  Exposed for unit testing.  Throws backup_auth_missing() on malformed input.
AzureAccessToken parseAzureTokenResponse(const std::string& jsonBody);

#endif
