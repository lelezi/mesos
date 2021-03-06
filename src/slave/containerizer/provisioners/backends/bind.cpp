/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <process/dispatch.hpp>
#include <process/process.hpp>

#include <stout/foreach.hpp>
#include <stout/os.hpp>

#include "linux/fs.hpp"

#include "slave/containerizer/provisioners/backends/bind.hpp"

using namespace process;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

class BindBackendProcess : public Process<BindBackendProcess>
{
public:
  Future<Nothing> provision(const vector<string>& layers, const string& rootfs);

  Future<bool> destroy(const string& rootfs);
};


Try<Owned<Backend>> BindBackend::create(const Flags&)
{
  Result<string> user = os::user();
  if (!user.isSome()) {
    return Error("Failed to determine user: " +
                 (user.isError() ? user.error() : "username not found"));
  }

  if (user.get() != "root") {
    return Error("BindBackend requires root privileges");
  }

  return Owned<Backend>(new BindBackend(
      Owned<BindBackendProcess>(new BindBackendProcess())));
}


BindBackend::~BindBackend()
{
  terminate(process.get());
  wait(process.get());
}


BindBackend::BindBackend(Owned<BindBackendProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Future<Nothing> BindBackend::provision(
    const vector<string>& layers,
    const string& rootfs)
{
  return dispatch(
      process.get(), &BindBackendProcess::provision, layers, rootfs);
}


Future<bool> BindBackend::destroy(const string& rootfs)
{
  return dispatch(process.get(), &BindBackendProcess::destroy, rootfs);
}


Future<Nothing> BindBackendProcess::provision(
    const vector<string>& layers,
    const string& rootfs)
{
  if (layers.size() > 1) {
    return Failure(
        "Multiple layers are not supported by the bind backend");
  }

  if (layers.size() == 0) {
    return Failure("No filesystem layer provided");
  }

  Try<Nothing> mkdir = os::mkdir(rootfs);
  if (mkdir.isError()) {
    return Failure("Failed to create container rootfs at " + rootfs);
  }

  // TODO(xujyan): Use MS_REC? Does any provisioner use mounts within
  // its image store in a single layer?
  Try<Nothing> mount = fs::mount(
      layers.front(),
      rootfs,
      None(),
      MS_BIND,
      NULL);

  if (mount.isError()) {
    return Failure(
        "Failed to bind mount rootfs '" + layers.front() +
        "' to '" + rootfs + "': " + mount.error());
  }

  // And remount it read-only.
  mount = fs::mount(
      None(), // Ignored.
      rootfs,
      None(),
      MS_BIND | MS_RDONLY | MS_REMOUNT,
      NULL);

  if (mount.isError()) {
    return Failure(
        "Failed to remount rootfs '" + rootfs + "' read-only: " +
        mount.error());
  }

  return Nothing();
}


Future<bool> BindBackendProcess::destroy(const string& rootfs)
{
  Try<fs::MountInfoTable> mountTable = fs::MountInfoTable::read();

  if (mountTable.isError()) {
    return Failure("Failed to read mount table: " + mountTable.error());
  }

  foreach (const fs::MountInfoTable::Entry& entry, mountTable.get().entries) {
    // TODO(xujyan): If MS_REC was used in 'provision()' we would need to
    // check `strings::startsWith(entry.target, rootfs)` here to unmount
    // all nested mounts.
    if (entry.target == rootfs) {
      // NOTE: This would fail if the rootfs is still in use.
      Try<Nothing> unmount = fs::unmount(entry.target);
      if (unmount.isError()) {
        return Failure(
            "Failed to destroy bind-mounted rootfs '" + rootfs + "': " +
            unmount.error());
      }

      Try<Nothing> rmdir = os::rmdir(rootfs);
      if (rmdir.isError()) {
        return Failure(
            "Failed to remove rootfs mount point '" + rootfs + "': " +
            rmdir.error());
      }

      return true;
    }
  }

  return false;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
