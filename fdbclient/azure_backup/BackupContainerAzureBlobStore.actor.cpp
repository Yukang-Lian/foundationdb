/*
 * BackupContainerAzureBlobStore.actor.cpp
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

#include "fdbclient/BackupContainerAzureBlobStore.h"
#include "fdbrpc/AsyncFileEncrypted.h"
#include <future>

#include "flow/actorcompiler.h" // This must be the last #include.

namespace {

std::string const notFoundErrorCode = "404";

void printAzureError(std::string const& operationName, azure::storage_lite::storage_error const& err) {
	printf("(%s) : Error from Azure SDK : %s (%s) : %s\n",
	       operationName.c_str(),
	       err.code_name.c_str(),
	       err.code.c_str(),
	       err.message.c_str());
}

template <class T>
T waitAzureFuture(std::future<azure::storage_lite::storage_outcome<T>>&& f, std::string const& operationName) {
	auto outcome = f.get();
	if (outcome.success()) {
		return outcome.response();
	} else {
		printAzureError(operationName, outcome.error());
		throw backup_error();
	}
}

} // namespace

class BackupContainerAzureBlobStoreImpl {
public:
	using AzureClient = azure::storage_lite::blob_client;

	class ReadFile final : public IAsyncFile, ReferenceCounted<ReadFile> {
		AsyncTaskThread* asyncTaskThread;
		std::string containerName;
		std::string blobName;
		std::shared_ptr<AzureClient> client;

	public:
		ReadFile(AsyncTaskThread& asyncTaskThread,
		         const std::string& containerName,
		         const std::string& blobName,
		         std::shared_ptr<AzureClient> const& client)
		  : asyncTaskThread(&asyncTaskThread), containerName(containerName), blobName(blobName), client(client) {}

		void addref() override { ReferenceCounted<ReadFile>::addref(); }
		void delref() override { ReferenceCounted<ReadFile>::delref(); }
		Future<int> read(void* data, int length, int64_t offset) override {
			TraceEvent(SevDebug, "BCAzureBlobStoreRead")
			    .detail("Length", length)
			    .detail("Offset", offset)
			    .detail("ContainerName", containerName)
			    .detail("BlobName", blobName);
			return asyncTaskThread->execAsync([client = this->client,
			                                   containerName = this->containerName,
			                                   blobName = this->blobName,
			                                   data,
			                                   length,
			                                   offset] {
				std::ostringstream oss(std::ios::out | std::ios::binary);
				waitAzureFuture(client->download_blob_to_stream(containerName, blobName, offset, length, oss),
				                "download_blob_to_stream");
				auto str = std::move(oss).str();
				memcpy(data, str.c_str(), str.size());
				return static_cast<int>(str.size());
			});
		}
		Future<Void> zeroRange(int64_t offset, int64_t length) override { throw file_not_writable(); }
		Future<Void> write(void const* data, int length, int64_t offset) override { throw file_not_writable(); }
		Future<Void> truncate(int64_t size) override { throw file_not_writable(); }
		Future<Void> sync() override { throw file_not_writable(); }
		Future<int64_t> size() const override {
			TraceEvent(SevDebug, "BCAzureBlobStoreReadFileSize")
			    .detail("ContainerName", containerName)
			    .detail("BlobName", blobName);
			return asyncTaskThread->execAsync(
			    [client = this->client, containerName = this->containerName, blobName = this->blobName] {
				    auto resp =
				        waitAzureFuture(client->get_blob_properties(containerName, blobName), "get_blob_properties");
				    return static_cast<int64_t>(resp.size);
			    });
		}
		std::string getFilename() const override { return blobName; }
		int64_t debugFD() const override { return 0; }
	};

	class WriteFile final : public IAsyncFile, ReferenceCounted<WriteFile> {
		AsyncTaskThread* asyncTaskThread;
		std::string containerName;
		std::string blobName;
		std::shared_ptr<AzureClient> client;
		int64_t m_cursor{ 0 };
		// Ideally this buffer should not be a string, but
		// the Azure SDK only supports/tests uploading to append
		// blobs from a stringstream.
		std::string buffer;

		static constexpr size_t bufferLimit = 1 << 20;

	public:
		WriteFile(AsyncTaskThread& asyncTaskThread,
		          const std::string& containerName,
		          const std::string& blobName,
		          std::shared_ptr<AzureClient> const& client)
		  : asyncTaskThread(&asyncTaskThread), containerName(containerName), blobName(blobName), client(client) {}

		void addref() override { ReferenceCounted<WriteFile>::addref(); }
		void delref() override { ReferenceCounted<WriteFile>::delref(); }
		Future<int> read(void* data, int length, int64_t offset) override { throw file_not_readable(); }
		Future<Void> write(void const* data, int length, int64_t offset) override {
			if (offset != m_cursor) {
				throw non_sequential_op();
			}
			m_cursor += length;
			auto p = static_cast<char const*>(data);
			buffer.append(p, length);
			if (buffer.size() > bufferLimit) {
				return sync();
			} else {
				return Void();
			}
		}
		Future<Void> truncate(int64_t size) override {
			if (size != m_cursor) {
				throw non_sequential_op();
			}
			return Void();
		}
		Future<Void> sync() override {
			TraceEvent(SevDebug, "BCAzureBlobStoreSync")
			    .detail("Length", buffer.size())
			    .detail("ContainerName", containerName)
			    .detail("BlobName", blobName);
			auto movedBuffer = std::move(buffer);
			buffer = {};
			if (!movedBuffer.empty()) {
				return asyncTaskThread->execAsync([client = this->client,
				                                   containerName = this->containerName,
				                                   blobName = this->blobName,
				                                   buffer = std::move(movedBuffer)] {
					std::istringstream iss(std::move(buffer));
					waitAzureFuture(client->append_block_from_stream(containerName, blobName, iss),
					                "append_block_from_stream");
					return Void();
				});
			}
			return Void();
		}
		Future<int64_t> size() const override {
			TraceEvent(SevDebug, "BCAzureBlobStoreSize")
			    .detail("ContainerName", containerName)
			    .detail("BlobName", blobName);
			return asyncTaskThread->execAsync(
			    [client = this->client, containerName = this->containerName, blobName = this->blobName] {
				    auto resp =
				        waitAzureFuture(client->get_blob_properties(containerName, blobName), "get_blob_properties");
				    return static_cast<int64_t>(resp.size);
			    });
		}
		std::string getFilename() const override { return blobName; }
		int64_t debugFD() const override { return -1; }
	};

	class BackupFile final : public IBackupFile, ReferenceCounted<BackupFile> {
		Reference<IAsyncFile> m_file;
		int64_t m_offset;

	public:
		BackupFile(const std::string& fileName, Reference<IAsyncFile> file)
		  : IBackupFile(fileName), m_file(file), m_offset(0) {}
		Future<Void> append(const void* data, int len) override {
			Future<Void> r = m_file->write(data, len, m_offset);
			m_offset += len;
			return r;
		}
		Future<Void> finish() override {
			Reference<BackupFile> self = Reference<BackupFile>::addRef(this);
			return map(m_file->sync(), [=](Void _) {
				self->m_file.clear();
				return Void();
			});
		}
		int64_t size() const override { return m_offset; }
		void addref() override { ReferenceCounted<BackupFile>::addref(); }
		void delref() override { ReferenceCounted<BackupFile>::delref(); }
	};

	static bool isDirectory(const std::string& blobName) { return blobName.size() && blobName.back() == '/'; }

	// Hack to get around the fact that macros don't work inside actor functions
	static Reference<IAsyncFile> encryptFile(Reference<IAsyncFile> const& f, AsyncFileEncrypted::Mode mode) {
		Reference<IAsyncFile> result = f;
		result = makeReference<AsyncFileEncrypted>(result, mode);
		return result;
	}

	ACTOR static Future<Reference<IAsyncFile>> readFile(BackupContainerAzureBlobStore* self, std::string fileName) {
		bool exists = wait(self->blobExists(fileName));
		if (!exists) {
			throw file_not_found();
		}
		Reference<IAsyncFile> f =
		    makeReference<ReadFile>(self->asyncTaskThread, self->containerName, self->blobPath(fileName), self->client);
		if (self->usesEncryption()) {
			f = encryptFile(f, AsyncFileEncrypted::Mode::READ_ONLY);
		}
		return f;
	}

	ACTOR static Future<Reference<IBackupFile>> writeFile(BackupContainerAzureBlobStore* self, std::string fileName) {
		TraceEvent(SevDebug, "BCAzureBlobStoreCreateWriteFile")
		    .detail("ContainerName", self->containerName)
		    .detail("FileName", fileName);
		wait(self->asyncTaskThread.execAsync(
		    [client = self->client, containerName = self->containerName, blobName = self->blobPath(fileName)] {
			    waitAzureFuture(client->create_append_blob(containerName, blobName), "create_append_blob");
			    return Void();
		    }));
		Reference<IAsyncFile> f = makeReference<WriteFile>(
		    self->asyncTaskThread, self->containerName, self->blobPath(fileName), self->client);
		if (self->usesEncryption()) {
			f = encryptFile(f, AsyncFileEncrypted::Mode::APPEND_ONLY);
		}
		return makeReference<BackupFile>(fileName, f);
	}

	static void listFiles(std::shared_ptr<AzureClient> const& client,
	                      const std::string& containerName,
	                      const std::string& path,
	                      std::function<bool(std::string const&)> folderPathFilter,
	                      BackupContainerFileSystem::FilesAndSizesT& result) {
		auto resp = waitAzureFuture(client->list_blobs_segmented(containerName, "/", "", path), "list_blobs_segmented");
		for (const auto& blob : resp.blobs) {
			if (isDirectory(blob.name) && (!folderPathFilter || folderPathFilter(blob.name))) {
				listFiles(client, containerName, blob.name, folderPathFilter, result);
			} else {
				result.emplace_back(blob.name, blob.content_length);
			}
		}
	}

	ACTOR static Future<Void> deleteContainer(BackupContainerAzureBlobStore* self, int* pNumDeleted) {
		if (!self->prefix.empty()) {
			// Shared-container layout: this backup owns only the blobs under its key prefix.
			// Delete those individually and never the container itself, which may hold other
			// backups under different prefixes.
			state BackupContainerFileSystem::FilesAndSizesT files = wait(self->listFiles());
			TraceEvent(SevDebug, "BCAzureBlobStoreDeletePrefix")
			    .detail("ContainerName", self->containerName)
			    .detail("Prefix", self->prefix)
			    .detail("FilesToDelete", files.size())
			    .detail("TrackNumDeleted", pNumDeleted != nullptr);
			state int i = 0;
			for (; i < files.size(); ++i) {
				wait(self->deleteFile(files[i].first));
			}
			if (pNumDeleted) {
				*pNumDeleted += files.size();
			}
			return Void();
		}
		state int filesToDelete = 0;
		if (pNumDeleted) {
			BackupContainerFileSystem::FilesAndSizesT numFiles = wait(self->listFiles());
			filesToDelete = numFiles.size();
		}
		TraceEvent(SevDebug, "BCAzureBlobStoreDeleteContainer")
		    .detail("FilesToDelete", filesToDelete)
		    .detail("ContainerName", self->containerName)
		    .detail("TrackNumDeleted", pNumDeleted != nullptr);
		wait(self->asyncTaskThread.execAsync([containerName = self->containerName, client = self->client] {
			waitAzureFuture(client->delete_container(containerName), "delete_container");
			return Void();
		}));
		if (pNumDeleted) {
			*pNumDeleted += filesToDelete;
		}
		return Void();
	}

	// Fetches an OAuth bearer token for Azure Storage and installs it into the container's
	// token_credential, then keeps refreshing it before it expires for the lifetime of the
	// container.  The blocking network calls run on the container's AsyncTaskThread.  The
	// lambda deliberately captures the credential shared_ptr and the mode by value rather
	// than self, so a task still queued while the container is being destroyed is safe.
	ACTOR static Future<Void> tokenRefreshLoop(BackupContainerAzureBlobStore* self) {
		state int consecutiveFailures = 0;
		loop {
			try {
				double expiresIn =
				    wait(self->asyncTaskThread.execAsync([mode = self->authMode, credential = self->tokenCredential] {
					    AzureAccessToken token = fetchAzureStorageToken(mode);
					    credential->set_token(token.token);
					    return token.expiresIn;
				    }));
				consecutiveFailures = 0;
				// Refresh five minutes before expiry, but not more than once a minute.
				state double refreshDelay = std::max(60.0, expiresIn - 300.0);
				TraceEvent("BCAzureBlobStoreTokenRefreshed")
				    .suppressFor(60)
				    .detail("ExpiresIn", expiresIn)
				    .detail("NextRefreshIn", refreshDelay);
				wait(delay(refreshDelay));
			} catch (Error& e) {
				if (e.code() == error_code_actor_cancelled) {
					throw;
				}
				++consecutiveFailures;
				TraceEvent(SevWarnAlways, "BCAzureBlobStoreTokenRefreshFailed")
				    .error(e)
				    .detail("ConsecutiveFailures", consecutiveFailures);
				wait(delay(std::min(10.0 * consecutiveFailures, 120.0)));
			}
		}
	}
};

Future<bool> BackupContainerAzureBlobStore::blobExists(const std::string& fileName) {
	TraceEvent(SevDebug, "BCAzureBlobStoreCheckExists")
	    .detail("FileName", fileName)
	    .detail("ContainerName", containerName);
	return asyncTaskThread.execAsync(
	    [client = this->client, containerName = this->containerName, blobName = blobPath(fileName)] {
		    auto outcome = client->get_blob_properties(containerName, blobName).get();
		    if (outcome.success()) {
			    return true;
		    } else {
			    auto const& err = outcome.error();
			    if (err.code == notFoundErrorCode) {
				    return false;
			    } else {
				    printAzureError("get_blob_properties", err);
				    throw backup_error();
			    }
		    }
	    });
}

std::string BackupContainerAzureBlobStore::normalizePrefix(std::string prefix) {
	size_t begin = prefix.find_first_not_of('/');
	if (begin == std::string::npos) {
		return "";
	}
	size_t end = prefix.find_last_not_of('/');
	prefix = prefix.substr(begin, end - begin + 1);

	auto invalidPrefix = [&]() {
		IBackupContainer::lastOpenError = "Invalid azure key prefix: '" + prefix + "'";
		throw backup_invalid_url();
	};
	auto validSegment = [](const std::string& segment) {
		return !segment.empty() && segment != "." && segment != "..";
	};
	auto isAsciiAlphanumeric = [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
	};

	// Reject empty ("//"), "." and ".." segments and unexpected characters so the prefix
	// always denotes a well-formed blob path that cannot traverse upward.
	std::string segment;
	for (auto c : prefix) {
		if (c == '/') {
			if (!validSegment(segment)) {
				invalidPrefix();
			}
			segment.clear();
			continue;
		}
		if (!isAsciiAlphanumeric(c) && c != '_' && c != '-' && c != '.') {
			invalidPrefix();
		}
		segment += c;
	}
	if (!validSegment(segment)) {
		invalidPrefix();
	}
	return prefix;
}

BackupContainerAzureBlobStore::BackupContainerAzureBlobStore(const std::string& endpoint,
                                                             const std::string& accountName,
                                                             const std::string& containerName,
                                                             const std::string& rawPrefix,
                                                             const Optional<std::string>& encryptionKeyFileName)
  : containerName(containerName), prefix(normalizePrefix(rawPrefix)) {
	setEncryptionKey(encryptionKeyFileName);
	authMode = azureAuthModeFromEnvironment();
	std::shared_ptr<azure::storage_lite::storage_credential> credential;
	if (authMode == AzureAuthMode::SHARED_KEY) {
		const char* _accountKey = std::getenv("AZURE_KEY");
		if (!_accountKey) {
			TraceEvent(SevError, "EnvironmentVariableNotFound").detail("EnvVariable", "AZURE_KEY");
			// TODO: More descriptive error?
			throw backup_error();
		}
		std::string accountKey = _accountKey;
		credential = std::make_shared<azure::storage_lite::shared_key_credential>(accountName, accountKey);
	} else {
		// OAuth bearer token authentication through the Azure Identity SDK (managed identity,
		// workload identity or the SDK's default credential chain).  The credential starts out
		// with an empty token; the initial fetch is queued as the first
		// task on this container's AsyncTaskThread, so every subsequent operation on the
		// container naturally runs after a token has been installed.
		tokenCredential = std::make_shared<azure::storage_lite::token_credential>("");
		credential = tokenCredential;
	}
	// FDB_AZURE_ALLOW_HTTP=1 selects plain http, for local testing (e.g. Azurite) and private
	// endpoints only.  Defaults to https.
	const char* allowHttp = std::getenv("FDB_AZURE_ALLOW_HTTP");
	bool useHttps = !(allowHttp != nullptr && std::string(allowHttp) == "1");
	auto storageAccount = std::make_shared<azure::storage_lite::storage_account>(
	    accountName, credential, useHttps, (useHttps ? "https://" : "http://") + endpoint);
	client = std::make_unique<AzureClient>(storageAccount, 1);
	if (tokenCredential != nullptr) {
		tokenRefreshFuture = BackupContainerAzureBlobStoreImpl::tokenRefreshLoop(this);
	}
}

void BackupContainerAzureBlobStore::addref() {
	return ReferenceCounted<BackupContainerAzureBlobStore>::addref();
}
void BackupContainerAzureBlobStore::delref() {
	return ReferenceCounted<BackupContainerAzureBlobStore>::delref();
}

Future<Void> BackupContainerAzureBlobStore::create() {
	TraceEvent(SevDebug, "BCAzureBlobStoreCreateContainer").detail("ContainerName", containerName);
	Future<Void> createContainerFuture =
	    asyncTaskThread.execAsync([containerName = this->containerName, client = this->client] {
		    auto outcome = client->get_container_properties(containerName).get();
		    if (!outcome.success()) {
			    waitAzureFuture(client->create_container(containerName), "create_container");
		    }
		    return Void();
	    });
	Future<Void> encryptionSetupFuture = usesEncryption() ? encryptionSetupComplete() : Void();
	return createContainerFuture && encryptionSetupFuture;
}

Future<bool> BackupContainerAzureBlobStore::exists() {
	TraceEvent(SevDebug, "BCAzureBlobStoreCheckContainerExists").detail("ContainerName", containerName);
	return asyncTaskThread.execAsync([containerName = this->containerName, client = this->client] {
		auto outcome = client->get_container_properties(containerName).get();
		if (outcome.success()) {
			return true;
		} else {
			auto const& err = outcome.error();
			if (err.code == notFoundErrorCode) {
				return false;
			} else {
				printAzureError("got_container_properties", err);
				throw backup_error();
			}
		}
	});
}

Future<Reference<IAsyncFile>> BackupContainerAzureBlobStore::readFile(const std::string& fileName) {
	return BackupContainerAzureBlobStoreImpl::readFile(this, fileName);
}

Future<Reference<IBackupFile>> BackupContainerAzureBlobStore::writeFile(const std::string& fileName) {
	return BackupContainerAzureBlobStoreImpl::writeFile(this, fileName);
}

Future<Void> BackupContainerAzureBlobStore::writeEntireFile(const std::string& fileName,
                                                            const std::string& fileContents) {
	return writeEntireFileFallback(fileName, fileContents);
}

Future<BackupContainerFileSystem::FilesAndSizesT> BackupContainerAzureBlobStore::listFiles(
    const std::string& path,
    std::function<bool(std::string const&)> folderPathFilter) {
	TraceEvent(SevDebug, "BCAzureBlobStoreListFiles").detail("ContainerName", containerName).detail("Path", path);
	return asyncTaskThread.execAsync([client = this->client,
	                                  containerName = this->containerName,
	                                  queryPath = blobPath(path),
	                                  stripLen = prefix.empty() ? size_t(0) : prefix.size() + 1,
	                                  folderPathFilter = folderPathFilter] {
		// The impl works on raw blob names; translate the filter's input and the results
		// back to container-relative names when a key prefix is in use.
		std::function<bool(std::string const&)> rawNameFilter = folderPathFilter;
		if (stripLen > 0 && folderPathFilter) {
			rawNameFilter = [folderPathFilter, stripLen](const std::string& folderPath) {
				return folderPathFilter(folderPath.substr(stripLen));
			};
		}
		FilesAndSizesT result;
		BackupContainerAzureBlobStoreImpl::listFiles(client, containerName, queryPath, rawNameFilter, result);
		if (stripLen > 0) {
			for (auto& file : result) {
				file.first = file.first.substr(stripLen);
			}
		}
		return result;
	});
}

Future<Void> BackupContainerAzureBlobStore::deleteFile(const std::string& fileName) {
	TraceEvent(SevDebug, "BCAzureBlobStoreDeleteFile")
	    .detail("ContainerName", containerName)
	    .detail("FileName", fileName);
	return asyncTaskThread.execAsync(
	    [containerName = this->containerName, blobName = blobPath(fileName), client = client]() {
		    client->delete_blob(containerName, blobName).wait();
		    return Void();
	    });
}

Future<Void> BackupContainerAzureBlobStore::deleteContainer(int* pNumDeleted) {
	return BackupContainerAzureBlobStoreImpl::deleteContainer(this, pNumDeleted);
}

Future<std::vector<std::string>> BackupContainerAzureBlobStore::listURLs(const std::string& baseURL) {
	// TODO: Implement this
	return std::vector<std::string>{};
}

std::string BackupContainerAzureBlobStore::getURLFormat() {
	return "azure://<accountname>@<endpoint>/<container>[/<key_prefix>]/ (Note: The optional <key_prefix> places all "
	       "of the backup's blobs under the given key prefix inside the container, so one container can hold multiple "
	       "backups.)";
}

namespace {

// The body of the Azure prefix unit test.  A plain function so the actor compiler does not
// transform it (the test is fully synchronous).
void testAzureBackupPrefix() {
	// Normalization: leading/trailing slashes are stripped, empty selects the original layout.
	ASSERT(BackupContainerAzureBlobStore::normalizePrefix("").empty());
	ASSERT(BackupContainerAzureBlobStore::normalizePrefix("/").empty());
	ASSERT(BackupContainerAzureBlobStore::normalizePrefix("///").empty());
	ASSERT(BackupContainerAzureBlobStore::normalizePrefix("a") == "a");
	ASSERT(BackupContainerAzureBlobStore::normalizePrefix("/a/b/") == "a/b");
	// Unlike the S3 blobstore container, Azure has no reserved top-level folder names.
	ASSERT(BackupContainerAzureBlobStore::normalizePrefix("data") == "data");
	ASSERT(BackupContainerAzureBlobStore::normalizePrefix("backups/x") == "backups/x");

	// Invalid values: empty, "." or ".." segments and disallowed characters.
	for (auto bad : { "a//b", ".", "..", "a/../b", "a/./b", "a b", "a?b", "a%2Fb" }) {
		try {
			BackupContainerAzureBlobStore::normalizePrefix(bad);
			ASSERT(false);
		} catch (Error& e) {
			ASSERT_EQ(e.code(), error_code_backup_invalid_url);
		}
	}
}

} // namespace

TEST_CASE("/backup/containers/azure/prefix") {
	testAzureBackupPrefix();
	return Void();
}
