/*
 * BackupContainerS3BlobStore.actor.cpp
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

#include "fdbclient/AsyncFileS3BlobStore.actor.h"
#include "fdbclient/BackupContainerS3BlobStore.h"
#include "fdbrpc/AsyncFileEncrypted.h"
#include "fdbrpc/AsyncFileReadAhead.actor.h"
#include "flow/UnitTest.h"
#include "flow/actorcompiler.h" // This must be the last #include.

class BackupContainerS3BlobStoreImpl {
public:
	// Backup files to under a single folder prefix with subfolders for each named backup
	static const std::string DATAFOLDER;

	// Indexfolder contains keys for which user-named backups exist.  Backup names can contain an arbitrary
	// number of slashes so the backup names are kept in a separate folder tree from their actual data.
	static const std::string INDEXFOLDER;

	ACTOR static Future<std::vector<std::string>> listURLs(Reference<S3BlobStoreEndpoint> bstore,
	                                                       std::string bucket,
	                                                       std::string prefix) {
		state std::string basePath = INDEXFOLDER + '/';
		state std::string params = format("bucket=%s", bucket.c_str());
		if (!prefix.empty()) {
			basePath = prefix + "/" + basePath;
			params += format("&prefix=%s", prefix.c_str());
		}
		S3BlobStoreEndpoint::ListResult contents = wait(bstore->listObjects(bucket, basePath));
		std::vector<std::string> results;
		for (const auto& f : contents.objects) {
			results.push_back(bstore->getResourceURL(f.name.substr(basePath.size()), params));
		}
		return results;
	}

	class BackupFile : public IBackupFile, ReferenceCounted<BackupFile> {
	public:
		BackupFile(std::string fileName, Reference<IAsyncFile> file)
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

		void addref() final { return ReferenceCounted<BackupFile>::addref(); }
		void delref() final { return ReferenceCounted<BackupFile>::delref(); }

	private:
		Reference<IAsyncFile> m_file;
		int64_t m_offset;
	};

	ACTOR static Future<BackupContainerFileSystem::FilesAndSizesT> listFiles(
	    Reference<BackupContainerS3BlobStore> bc,
	    std::string path,
	    std::function<bool(std::string const&)> pathFilter) {
		// pathFilter expects container based paths, so create a wrapper which converts a raw path
		// to a container path by removing the known backup name prefix.
		state int prefixTrim = bc->dataPath("").size();
		std::function<bool(std::string const&)> rawPathFilter = [=](const std::string& folderPath) {
			ASSERT(folderPath.size() >= prefixTrim);
			return pathFilter(folderPath.substr(prefixTrim));
		};

		state S3BlobStoreEndpoint::ListResult result = wait(bc->m_bstore->listObjects(
		    bc->m_bucket, bc->dataPath(path), '/', std::numeric_limits<int>::max(), rawPathFilter));
		BackupContainerFileSystem::FilesAndSizesT files;
		for (const auto& o : result.objects) {
			ASSERT(o.name.size() >= prefixTrim);
			files.push_back({ o.name.substr(prefixTrim), o.size });
		}
		return files;
	}

	ACTOR static Future<Void> create(Reference<BackupContainerS3BlobStore> bc) {
		wait(bc->m_bstore->createBucket(bc->m_bucket));

		// Check/create the index entry
		bool exists = wait(bc->m_bstore->objectExists(bc->m_bucket, bc->indexEntry()));
		if (!exists) {
			wait(bc->m_bstore->writeEntireFile(bc->m_bucket, bc->indexEntry(), ""));
		}

		if (bc->usesEncryption()) {
			wait(bc->encryptionSetupComplete());
		}

		return Void();
	}

	ACTOR static Future<Void> deleteContainer(Reference<BackupContainerS3BlobStore> bc, int* pNumDeleted) {
		bool e = wait(bc->exists());
		if (!e) {
			TraceEvent(SevWarnAlways, "BackupContainerDoesNotExist").detail("URL", bc->getURL());
			throw backup_does_not_exist();
		}

		// First delete everything under the data prefix in the bucket
		wait(bc->m_bstore->deleteRecursively(bc->m_bucket, bc->dataPath(""), pNumDeleted));

		// Now that all files are deleted, delete the index entry
		wait(bc->m_bstore->deleteObject(bc->m_bucket, bc->indexEntry()));

		return Void();
	}
};

const std::string BackupContainerS3BlobStoreImpl::DATAFOLDER = "data";
const std::string BackupContainerS3BlobStoreImpl::INDEXFOLDER = "backups";

std::string BackupContainerS3BlobStore::dataPath(const std::string& path) {
	// if a key prefix was given, the entire layout lives under it.
	// if backup, include the backup data prefix.
	// if m_name ends in a trailing slash, don't add another
	std::string dataPath = "";
	if (!m_prefix.empty()) {
		dataPath = m_prefix + "/";
	}
	if (isBackup) {
		dataPath += BackupContainerS3BlobStoreImpl::DATAFOLDER + "/";
	}
	if (!m_name.empty() && m_name.back() == '/') {
		dataPath += m_name + path;
	} else {
		dataPath += m_name + "/" + path;
	}
	return dataPath;
}

// Get the path of the backups's index entry
std::string BackupContainerS3BlobStore::indexEntry() {
	ASSERT(isBackup);
	std::string path = BackupContainerS3BlobStoreImpl::INDEXFOLDER + "/" + m_name;
	if (!m_prefix.empty()) {
		path = m_prefix + "/" + path;
	}
	return path;
}

BackupContainerS3BlobStore::BackupContainerS3BlobStore(Reference<S3BlobStoreEndpoint> bstore,
                                                       const std::string& name,
                                                       const S3BlobStoreEndpoint::ParametersT& params,
                                                       const Optional<std::string>& encryptionKeyFileName,
                                                       bool isBackup)
  : m_bstore(bstore), m_name(name), m_bucket("FDB_BACKUPS_V2"), isBackup(isBackup) {
	setEncryptionKey(encryptionKeyFileName);
	// Two parameters are supported, "bucket" and "prefix"
	for (const auto& [name, value] : params) {
		if (name == "bucket") {
			m_bucket = value;
			continue;
		}
		if (name == "prefix") {
			m_prefix = normalizePrefix(value, &IBackupContainer::lastOpenError);
			continue;
		}
		TraceEvent(SevWarn, "BackupContainerS3BlobStoreInvalidParameter").detail("Name", name).detail("Value", value);
		IBackupContainer::lastOpenError = format("Unknown URL parameter: '%s'", name.c_str());
		throw backup_invalid_url();
	}
}

void BackupContainerS3BlobStore::addref() {
	return ReferenceCounted<BackupContainerS3BlobStore>::addref();
}
void BackupContainerS3BlobStore::delref() {
	return ReferenceCounted<BackupContainerS3BlobStore>::delref();
}

std::string BackupContainerS3BlobStore::getURLFormat() {
	return S3BlobStoreEndpoint::getURLFormat(true) +
	       " (Note: The 'bucket' parameter is required.  The optional 'prefix' parameter places the backup's "
	       "data and index folder trees under the given object key prefix instead of the bucket root.)";
}

Future<Reference<IAsyncFile>> BackupContainerS3BlobStore::readFile(const std::string& path) {
	Reference<IAsyncFile> f = makeReference<AsyncFileS3BlobStoreRead>(m_bstore, m_bucket, dataPath(path));

	if (usesEncryption()) {
		f = makeReference<AsyncFileEncrypted>(f, AsyncFileEncrypted::Mode::READ_ONLY);
	}
	if (m_bstore->knobs.enable_read_cache) {
		f = makeReference<AsyncFileReadAheadCache>(f,
		                                           m_bstore->knobs.read_block_size,
		                                           m_bstore->knobs.read_ahead_blocks,
		                                           m_bstore->knobs.concurrent_reads_per_file,
		                                           m_bstore->knobs.read_cache_blocks_per_file);
	}
	return f;
}

Future<std::vector<std::string>> BackupContainerS3BlobStore::listURLs(Reference<S3BlobStoreEndpoint> bstore,
                                                                      const std::string& bucket,
                                                                      const std::string& prefix) {
	return BackupContainerS3BlobStoreImpl::listURLs(bstore, bucket, prefix);
}

Future<Reference<IBackupFile>> BackupContainerS3BlobStore::writeFile(const std::string& path) {
	Reference<IAsyncFile> f = makeReference<AsyncFileS3BlobStoreWrite>(m_bstore, m_bucket, dataPath(path));
	if (usesEncryption()) {
		f = makeReference<AsyncFileEncrypted>(f, AsyncFileEncrypted::Mode::APPEND_ONLY);
	}
	return Future<Reference<IBackupFile>>(makeReference<BackupContainerS3BlobStoreImpl::BackupFile>(path, f));
}

Future<Void> BackupContainerS3BlobStore::writeEntireFile(const std::string& path, const std::string& fileContents) {
	return m_bstore->writeEntireFile(m_bucket, dataPath(path), fileContents);
}

Future<Void> BackupContainerS3BlobStore::deleteFile(const std::string& path) {
	return m_bstore->deleteObject(m_bucket, dataPath(path));
}

Future<BackupContainerFileSystem::FilesAndSizesT> BackupContainerS3BlobStore::listFiles(
    const std::string& path,
    std::function<bool(std::string const&)> pathFilter) {
	return BackupContainerS3BlobStoreImpl::listFiles(
	    Reference<BackupContainerS3BlobStore>::addRef(this), path, pathFilter);
}

Future<Void> BackupContainerS3BlobStore::create() {
	return BackupContainerS3BlobStoreImpl::create(Reference<BackupContainerS3BlobStore>::addRef(this));
}

Future<bool> BackupContainerS3BlobStore::exists() {
	return m_bstore->objectExists(m_bucket, indexEntry());
}

Future<Void> BackupContainerS3BlobStore::deleteContainer(int* pNumDeleted) {
	return BackupContainerS3BlobStoreImpl::deleteContainer(Reference<BackupContainerS3BlobStore>::addRef(this),
	                                                       pNumDeleted);
}

std::string BackupContainerS3BlobStore::getBucket() const {
	return m_bucket;
}

std::string BackupContainerS3BlobStore::getPrefix() const {
	return m_prefix;
}

namespace {

// The body of the prefix unit test.  A plain function so the actor compiler does not
// transform it (the test is fully synchronous).
void testBlobstoreBackupPrefix() {
	// Normalization: leading/trailing slashes are stripped, empty selects the default layout.
	ASSERT(BackupContainerS3BlobStore::normalizePrefix("").empty());
	ASSERT(BackupContainerS3BlobStore::normalizePrefix("/").empty());
	ASSERT(BackupContainerS3BlobStore::normalizePrefix("///").empty());
	ASSERT(BackupContainerS3BlobStore::normalizePrefix("a") == "a");
	ASSERT(BackupContainerS3BlobStore::normalizePrefix("/a/b/") == "a/b");
	ASSERT(BackupContainerS3BlobStore::normalizePrefix("tenant/data") == "tenant/data");
	ASSERT(BackupContainerS3BlobStore::normalizePrefix("my_instance/backups") == "my_instance/backups");
	ASSERT(BackupContainerS3BlobStore::normalizePrefix("v1/backups/backup-001.fdb") == "v1/backups/backup-001.fdb");

	// Invalid values: reserved first segments, empty, "." or ".." segments and disallowed characters.
	for (auto bad : { "data",
	                  "/data/x/",
	                  "backups",
	                  "/backups/x/",
	                  "a//b",
	                  ".",
	                  "..",
	                  "a/../b",
	                  "a/./b",
	                  "a b",
	                  "a?b",
	                  "a%2Fb" }) {
		try {
			BackupContainerS3BlobStore::normalizePrefix(bad);
			ASSERT(false);
		} catch (Error& e) {
			ASSERT_EQ(e.code(), error_code_backup_invalid_url);
		}
	}

	// URL parameter parsing: "prefix" is accepted and normalized, unknown parameters still throw.
	std::string resource;
	std::string error;
	S3BlobStoreEndpoint::ParametersT backupParams;
	Reference<S3BlobStoreEndpoint> bstore = S3BlobStoreEndpoint::fromString(
	    "blobstore://ak:sk@localhost:9999/some/container?bucket=bkt&region=us-east-1&prefix=old&prefix=/p1/p2/",
	    {},
	    &resource,
	    &error,
	    &backupParams);
	ASSERT(bstore.isValid());
	ASSERT(resource == "some/container");
	auto c = makeReference<BackupContainerS3BlobStore>(bstore, resource, backupParams, Optional<std::string>(), true);
	ASSERT(c->getBucket() == "bkt");
	ASSERT(c->getPrefix() == "p1/p2");

	// HTML entity encoded ampersands are not decoded: "amp;prefix" is an unknown parameter and is rejected.
	std::string htmlResource;
	S3BlobStoreEndpoint::ParametersT htmlParams;
	Reference<S3BlobStoreEndpoint> htmlStore =
	    S3BlobStoreEndpoint::fromString("blobstore://localhost:9999/some/container?bucket=bkt&region=us-east-1&amp;"
	                                    "prefix=p",
	                                    {},
	                                    &htmlResource,
	                                    &error,
	                                    &htmlParams);
	ASSERT(htmlStore.isValid());
	try {
		makeReference<BackupContainerS3BlobStore>(htmlStore, htmlResource, htmlParams, Optional<std::string>(), true);
		ASSERT(false);
	} catch (Error& e) {
		ASSERT_EQ(e.code(), error_code_backup_invalid_url);
	}

	S3BlobStoreEndpoint::ParametersT badParams = backupParams;
	badParams["prefix"] = "has space";
	try {
		makeReference<BackupContainerS3BlobStore>(bstore, resource, badParams, Optional<std::string>(), true);
		ASSERT(false);
	} catch (Error& e) {
		ASSERT_EQ(e.code(), error_code_backup_invalid_url);
	}
}

} // namespace

TEST_CASE("/backup/containers/blobstore/prefix") {
	testBlobstoreBackupPrefix();
	return Void();
}
