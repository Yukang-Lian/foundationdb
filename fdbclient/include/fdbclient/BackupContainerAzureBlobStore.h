/*
 * BackupContainerAzureBlobStore.h
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

#if (!defined FDBCLIENT_BACKUP_CONTAINER_AZURE_BLOBSTORE_H) && (defined BUILD_AZURE_BACKUP)
#define FDBCLIENT_BACKUP_CONTAINER_AZURE_BLOBSTORE_H
#pragma once

#include "fdbclient/AsyncTaskThread.h"
#include "fdbclient/AzureTokenProvider.h"
#include "fdbclient/BackupContainerFileSystem.h"

#include "blob/blob_client.h"

class BackupContainerAzureBlobStore final : public BackupContainerFileSystem,
                                            ReferenceCounted<BackupContainerAzureBlobStore> {
	using AzureClient = azure::storage_lite::blob_client;

	std::shared_ptr<AzureClient> client;
	std::string containerName;

	// Optional object key prefix under which all of this backup's blobs are placed inside the
	// container.  Normalized to contain no leading or trailing slash.  Empty selects the
	// original one-backup-per-container layout.
	std::string prefix;

	AsyncTaskThread asyncTaskThread;

	// Maps a container-relative file name to the actual blob name, applying the optional key
	// prefix.  With a non-empty prefix and an empty fileName this yields "<prefix>/", the
	// listing query prefix for the entire backup.
	std::string blobPath(const std::string& fileName) const {
		return prefix.empty() ? fileName : prefix + "/" + fileName;
	}

	// How this container authenticates against Azure Storage.  See AzureTokenProvider.h.
	AzureAuthMode authMode{ AzureAuthMode::SHARED_KEY };

	// Non-null when authMode uses OAuth bearer tokens.  The token value is refreshed by
	// tokenRefreshFuture's actor via set_token().
	std::shared_ptr<azure::storage_lite::token_credential> tokenCredential;

	Future<bool> blobExists(const std::string& fileName);

	friend class BackupContainerAzureBlobStoreImpl;

	// Keeps the bearer token fresh for the lifetime of this container.  Must be the last
	// member so its destruction (which cancels the refresh actor) happens before the members
	// the actor uses are destroyed.
	Future<Void> tokenRefreshFuture;

public:
	BackupContainerAzureBlobStore(const std::string& endpoint,
	                              const std::string& accountName,
	                              const std::string& containerName,
	                              const std::string& rawPrefix,
	                              const Optional<std::string>& encryptionKeyFileName);

	// Normalize and validate an object key prefix taken from the path portion of an azure://
	// URL.  Strips leading and trailing slashes; an empty result selects the original
	// one-backup-per-container layout.  Throws backup_invalid_url if a path segment contains
	// characters outside [A-Za-z0-9._-] or a segment is empty, "." or "..".
	static std::string normalizePrefix(std::string prefix);

	void addref() override;
	void delref() override;

	Future<Void> create() override;

	Future<bool> exists() override;

	Future<Reference<IAsyncFile>> readFile(const std::string& fileName) override;

	Future<Reference<IBackupFile>> writeFile(const std::string& fileName) override;

	Future<Void> writeEntireFile(const std::string& fileName, const std::string& fileContents) override;

	Future<FilesAndSizesT> listFiles(const std::string& path = "",
	                                 std::function<bool(std::string const&)> folderPathFilter = nullptr) override;

	Future<Void> deleteFile(const std::string& fileName) override;

	Future<Void> deleteContainer(int* pNumDeleted) override;

	static Future<std::vector<std::string>> listURLs(const std::string& baseURL);

	static std::string getURLFormat();
};

#endif
