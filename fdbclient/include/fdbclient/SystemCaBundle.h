/*
 * SystemCaBundle.h
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

#ifndef FDBCLIENT_SYSTEM_CA_BUNDLE_H
#define FDBCLIENT_SYSTEM_CA_BUNDLE_H
#pragma once

#include <string>

#include "flow/Platform.h"

// The cloud SDKs talk TLS to their token endpoints through their own libcurl, which needs a CA bundle. This finds
// the system's, wherever the distribution keeps it. Empty if none is found.
inline std::string systemCaBundle() {
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

#endif
