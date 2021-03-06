/*  Copyright 2013 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.maidsafe.net/licenses

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

#include "maidsafe/nfs/client/maid_node_nfs.h"

#include "maidsafe/common/error.h"
#include "maidsafe/common/log.h"

#include "maidsafe/nfs/vault/account_creation.h"
#include "maidsafe/nfs/vault/account_removal.h"
#include "maidsafe/nfs/vault/pmid_registration.h"

namespace maidsafe {

namespace nfs_client {

void CreateAccount(std::shared_ptr<passport::Maid> maid,
                   std::shared_ptr<passport::Anmaid> anmaid,
                   std::shared_ptr<MaidNodeNfs> client_nfs) {
  passport::PublicMaid public_maid(*maid);
  {
    passport::PublicAnmaid public_anmaid(*anmaid);
    auto future(client_nfs->CreateAccount(nfs_vault::AccountCreation(public_maid,
                                                                     public_anmaid)));
    auto status(future.wait_for(boost::chrono::seconds(10)));
    if (status == boost::future_status::timeout) {
      std::cout << "can't create account" << std::endl;
      BOOST_THROW_EXCEPTION(MakeError(VaultErrors::failed_to_handle_request));
    }
    if (future.has_exception()) {
      try {
        future.get();
      } catch (const maidsafe_error& error) {
        if (error.code() == make_error_code(VaultErrors::account_already_exists) ||
            error.code() == make_error_code(VaultErrors::unique_data_clash))
          std::cout << "account already existed" << std::endl;
      } catch (...) {
        std::cout << "caught an unknown exception" << std::endl;
      }
      return;
    }
  }
  // waiting for syncs resolved
  boost::this_thread::sleep_for(boost::chrono::seconds(5));
  std::cout << "Account created for maid " << HexSubstr(public_maid.name()->string())
            << std::endl;
}

MaidNodeNfs::MaidNodeNfs(AsioService& asio_service, routing::Routing& routing,
                         passport::PublicPmid::Name pmid_node_hint)
    : get_timer_(asio_service),
      put_timer_(asio_service),
      get_versions_timer_(asio_service),
      get_branch_timer_(asio_service),
      create_account_timer_(asio_service),
      pmid_health_timer_(asio_service),
      create_version_tree_timer_(asio_service),
      put_version_timer_(asio_service),
      register_pmid_timer_(asio_service),
      dispatcher_(routing),
      service_([&]()->std::unique_ptr<MaidNodeService> {
        std::unique_ptr<MaidNodeService> service(
            new MaidNodeService(routing, get_timer_, put_timer_, get_versions_timer_,
                                get_branch_timer_, create_account_timer_, pmid_health_timer_,
                                create_version_tree_timer_, put_version_timer_,
                                register_pmid_timer_, get_handler_));
        return std::move(service);
      }()),
      pmid_node_hint_mutex_(),
      pmid_node_hint_(pmid_node_hint),
      get_handler_(get_timer_, dispatcher_) {}

passport::PublicPmid::Name MaidNodeNfs::pmid_node_hint() const {
  std::lock_guard<std::mutex> lock(pmid_node_hint_mutex_);
  return pmid_node_hint_;
}

void MaidNodeNfs::set_pmid_node_hint(const passport::PublicPmid::Name& pmid_node_hint) {
  std::lock_guard<std::mutex> lock(pmid_node_hint_mutex_);
  pmid_node_hint_ = pmid_node_hint;
}

boost::future<void> MaidNodeNfs::CreateAccount(
    const nfs_vault::AccountCreation& account_creation,
    const std::chrono::steady_clock::duration& timeout) {
  typedef MaidNodeService::CreateAccountResponse::Contents ResponseContents;
  auto promise(std::make_shared<boost::promise<void>>());
  auto response_functor([promise](const ResponseContents &result) {
      HandleCreateAccountResult(result, promise);
  });
  auto op_data(std::make_shared<nfs::OpData<ResponseContents>>(
      routing::Parameters::group_size - 1, response_functor));
  auto task_id(create_account_timer_.NewTaskId());
  create_account_timer_.AddTask(
      timeout, [op_data](ResponseContents create_account_response) {
                 op_data->HandleResponseContents(std::move(create_account_response));
               },
      // TODO(Fraser#5#): 2013-08-18 - Confirm expected count
      routing::Parameters::group_size * 2, task_id);
  dispatcher_.SendCreateAccountRequest(task_id, account_creation);
  return promise->get_future();
}

void MaidNodeNfs::RemoveAccount(const nfs_vault::AccountRemoval& account_removal) {
  dispatcher_.SendRemoveAccountRequest(account_removal);
}

boost::future<void> MaidNodeNfs::RegisterPmid(
    const nfs_vault::PmidRegistration& pmid_registration,
    const std::chrono::steady_clock::duration& timeout) {
  typedef MaidNodeService::RegisterPmidResponse::Contents ResponseContents;
  auto promise(std::make_shared<boost::promise<void>>());
  auto response_functor([promise](const ResponseContents &result) {
      HandleRegisterPmidResult(result, promise);
  });
  auto op_data(std::make_shared<nfs::OpData<ResponseContents>>(
      routing::Parameters::group_size - 1, response_functor));
  auto task_id(create_account_timer_.NewTaskId());
  register_pmid_timer_.AddTask(
      timeout, [op_data](ResponseContents create_account_response) {
                 op_data->HandleResponseContents(std::move(create_account_response));
               },
      // TODO(Mahmoud): Confirm expected count
      routing::Parameters::group_size - 1, task_id);
  dispatcher_.SendRegisterPmidRequest(task_id, pmid_registration);
  return promise->get_future();
}

void MaidNodeNfs::UnregisterPmid(const passport::PublicPmid::Name& pmid_name) {
  dispatcher_.SendUnregisterPmidRequest(pmid_name);
}

MaidNodeNfs::PmidHealthFuture MaidNodeNfs::GetPmidHealth(
    const passport::PublicPmid::Name& pmid_name,
    const std::chrono::steady_clock::duration& timeout) {
  typedef MaidNodeService::PmidHealthResponse::Contents ResponseContents;
  auto promise(std::make_shared<boost::promise<uint64_t>>());
  auto response_functor([promise](const ResponseContents& result) {
      HandlePmidHealthResult(result, promise);
  });
  auto op_data(std::make_shared<nfs::OpData<ResponseContents>>(
      routing::Parameters::group_size - 1, response_functor));
  auto task_id(pmid_health_timer_.NewTaskId());
  pmid_health_timer_.AddTask(
      timeout, [op_data](ResponseContents pmid_health_response) {
                 op_data->HandleResponseContents(std::move(pmid_health_response));
               },
      // TODO(Fraser#5#): 2013-08-18 - Confirm expected count
      routing::Parameters::group_size - 1, task_id);
  dispatcher_.SendPmidHealthRequest(task_id, pmid_name);
  return promise->get_future();
}

}  // namespace nfs_client

}  // namespace maidsafe
