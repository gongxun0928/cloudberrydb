/*-------------------------------------------------------------------------
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * pax_visimap.h
 *
 * IDENTIFICATION
 *	  contrib/pax_storage/src/cpp/access/pax_visimap.h
 *
 *-------------------------------------------------------------------------
 */

#pragma once

#include "comm/cbdb_api.h"

#include <memory>
#include <string>
#include <vector>

#include "storage/file_system.h"

namespace pax {
extern std::shared_ptr<std::vector<uint8>> LoadVisimap(
    FileSystem *fs, const std::shared_ptr<FileSystemOptions> &options,
    const std::string &visimap_file_path);
extern bool TestVisimap(Relation rel, const char *visimap_name, int offset);

}  // namespace pax
