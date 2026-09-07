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
// SHARED_KEY is the original behavior: the storage account key is taken from the AZURE_KEY
// environment variable and requests are signed with it.
//
// The other modes obtain OAuth bearer tokens for https://storage.azure.com/ through the Azure
// Identity SDK (azure-identity), mirroring how the S3 backup delegates credential resolution
// to the AWS SDK (sdk_auth):
//   MANAGED_IDENTITY   Azure::Identity::ManagedIdentityCredential: IMDS on VMs/VMSS, plus the
//                      App Service, Azure Arc, Service Fabric and Cloud Shell identity
//                      endpoints the SDK detects from their standard environment variables.
//                      FDB_AZURE_CLIENT_ID selects a user-assigned identity.
//   WORKLOAD_IDENTITY  Azure::Identity::WorkloadIdentityCredential: the federated service
//                      account token projected by Azure Workload Identity (e.g. on AKS), via
//                      the standard AZURE_CLIENT_ID, AZURE_TENANT_ID, AZURE_FEDERATED_TOKEN_FILE
//                      and AZURE_AUTHORITY_HOST variables.
//   DEFAULT            Azure::Identity::DefaultAzureCredential: the SDK's standard chain
//                      (environment client secret/certificate, workload identity, managed
//                      identity, Azure CLI).
//
// The mode is selected explicitly with the FDB_AZURE_AUTH_MODE environment variable
// ("shared_key", "managed_identity", "workload_identity" or "default").  An unset variable
// selects SHARED_KEY, the historical behavior.
enum class AzureAuthMode { SHARED_KEY, MANAGED_IDENTITY, WORKLOAD_IDENTITY, DEFAULT };

struct AzureAccessToken {
	std::string token;
	// Seconds until the token expires, relative to the moment it was fetched.
	double expiresIn = 0;
};

// Reads FDB_AZURE_AUTH_MODE and returns the selected mode.  Throws backup_invalid_url on an
// unrecognized value.
AzureAuthMode azureAuthModeFromEnvironment();

// Parses a mode name as accepted by FDB_AZURE_AUTH_MODE.  Returns false for unknown names.
bool parseAzureAuthMode(const std::string& name, AzureAuthMode& mode);

// Fetches a bearer token for scope https://storage.azure.com/.default using the given mode,
// which must not be SHARED_KEY.  This is a synchronous, blocking call into the Azure Identity
// SDK: it must run on a background thread (the backup container's AsyncTaskThread), never on
// the flow network thread.  Throws backup_auth_missing() on failure.
AzureAccessToken fetchAzureStorageToken(AzureAuthMode mode);

#endif
