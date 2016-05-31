
// Copyright (c) 2016, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "container_manager.h"
#include "util/path_tree.h"
#include "thread.h"

#include "boost/bind.hpp"
#include <glog/logging.h>

namespace baidu {
namespace galaxy {
namespace container {

ContainerManager::ContainerManager(boost::shared_ptr<baidu::galaxy::resource::ResourceManager> resman) :
    res_man_(resman),
    running_(false)
{
    assert(NULL != resman);
}

ContainerManager::~ContainerManager()
{
    running_ = false;
}

void ContainerManager::Setup() {
    std::string path = baidu::galaxy::path::RootPath();
    path += "/data";
    baidu::galaxy::util::ErrorCode ec = serializer_.Setup(path);
    if (ec.Code() != 0) {
        LOG(FATAL) << "set up serilzer failed, path is" << path 
            << " reason is: " << ec.Message();
        exit(-1);
    }
    LOG(INFO) << "succeed in setting up serialize db: " << path;

    int ret = Reload();
    if (0 != ret) {
        LOG(WARNING) << "failed in recovering container from meta, agent will exit";
        exit(-1);
    } else {
        LOG(INFO) <<"succeed in recovering container from meta";
    }

    running_ = true;
    this->keep_alive_thread_.Start(boost::bind(&ContainerManager::KeepAliveRoutine, this));
}

void ContainerManager::KeepAliveRoutine() {
    while (running_) {
        {
            boost::mutex::scoped_lock lock(mutex_);
            std::map<ContainerId, boost::shared_ptr<baidu::galaxy::container::Container> >::iterator iter = work_containers_.begin();
            while (iter != work_containers_.end()) {
                iter->second->KeepAlive();
                iter++;
            }
        }
        ::sleep(1);
    }
}

int ContainerManager::CreateContainer(const ContainerId& id, const baidu::galaxy::proto::ContainerDescription& desc)
{
    // enter creating stage, every time only one thread does creating
    baidu::galaxy::util::ErrorCode ec = stage_.EnterCreatingStage(id.SubId());
    if (ec.Code() == baidu::galaxy::util::kErrorRepeated) {
        LOG(WARNING) << "container " << id.CompactId() << " has been in creating stage already: " << ec.Message();
        return 0;
    }

    if (ec.Code() != baidu::galaxy::util::kErrorOk) {
        LOG(WARNING) << "container " << id.CompactId() << " enter create stage failed: " << ec.Message();
        return -1;
    }

    // judge container is exist or not
    {
        boost::mutex::scoped_lock lock(mutex_);
        std::map<ContainerId, boost::shared_ptr<Container> >::const_iterator iter
            = work_containers_.find(id);

        if (work_containers_.end() != iter) {
            LOG(INFO) << "container " << id.CompactId() << " has already been created";
            stage_.LeaveCreatingStage(id.SubId());
            return 0;
        }
    }

    // allcate resource
    ec = res_man_->Allocate(desc);
    if (0 != ec.Code()) {
        LOG(WARNING) << "fail in allocating resource for container "
                     << id.CompactId() << ", detail reason is: "
                     << ec.Message();

        stage_.LeaveCreatingStage(id.SubId());
        return -1;
    } else {
        LOG(INFO) << "succeed in allocating resource for " << id.CompactId();
    }

    // create container
    int ret = CreateContainer_(id, desc);

    if (0 != ret) {
        ec = res_man_->Release(desc);
        if (ec.Code() != 0) {
            LOG(FATAL) << "failed in releasing resource for container "
                       << id.CompactId() << ", detail reason is: "
                       << ec.Message();
        }
    } 
    stage_.LeaveCreatingStage(id.SubId());
    return ret;
}

int ContainerManager::ReleaseContainer(const ContainerId& id)
{
    // enter destroying stage, only one thread do releasing at a moment
    baidu::galaxy::util::ErrorCode ec = stage_.EnterDestroyingStage(id.SubId());
    if (ec.Code() == baidu::galaxy::util::kErrorRepeated) {
        LOG(INFO) << "container " << id.CompactId()
                  << " has been in destroying stage: " << ec.Message();
        return 0;
    }

    if (ec.Code() != baidu::galaxy::util::kErrorOk) {
        LOG(WARNING) << "failed in entering destroying stage for container " << id.CompactId()
                     << ", reason: " << ec.Message();
        return -1;
    }

    // judge container existence
    std::map<ContainerId, boost::shared_ptr<baidu::galaxy::container::Container> >::iterator iter;
    {
        boost::mutex::scoped_lock lock(mutex_);
        iter = work_containers_.find(id);

        if (work_containers_.end() == iter) {
            stage_.LeaveDestroyingStage(id.SubId());
            LOG(WARNING) << "container " << id.CompactId() << " do not exist";
            return 0;
        }
    }

    // do releasing
    int ret = 0;
    if (0 == (ret = iter->second->Destroy())) {

        ec = res_man_->Release(iter->second->Description());
        if (0 != ec.Code()) {
            LOG(FATAL) << "failed in releasing resource for container " << id.CompactId()
                       << " reason is: " << ec.Message();
            exit(-1);
        } else {
            LOG(INFO) << "succeed in releasing resource for container " << id.CompactId();
        }

        work_containers_.erase(iter);
        ec = serializer_.DeleteWork(id.GroupId(), id.SubId());
        if (ec.Code() != 0) {
            LOG(WARNING) << "delete key failed, key is: " << id.CompactId()
                << " reason is: " << ec.Message();
            ret = -1;
        } else {
            LOG(INFO) << "succeed in deleting container meta for container " << id.CompactId();
        }
    }
    stage_.LeaveDestroyingStage(id.SubId());

    LOG(INFO) << id.CompactId() << " leave destroying stage";
    return ret;
}

int ContainerManager::CreateContainer_(const ContainerId& id,
        const baidu::galaxy::proto::ContainerDescription& desc)
{

    boost::shared_ptr<baidu::galaxy::container::Container>
    container(new baidu::galaxy::container::Container(id, desc));

    if (0 != container->Construct()) {
        LOG(WARNING) << "fail in constructing container " << id.CompactId();
        return -1;
    }

    baidu::galaxy::util::ErrorCode ec = serializer_.SerializeWork(container->ContainerMeta());
    if (ec.Code() != 0) {
        LOG(WARNING) << "fail in serializing container meta " << id.CompactId();
        return -1;
    }
    LOG(INFO) << "succeed in serializing container meta for cantainer " << id.CompactId();

    {
        boost::mutex::scoped_lock lock(mutex_);
        work_containers_[id] = container;
        LOG(INFO) << "succeed in constructing container " << id.CompactId();
    }

    return 0;
}

void ContainerManager::ListContainers(std::vector<boost::shared_ptr<baidu::galaxy::proto::ContainerInfo> >& cis, bool fullinfo)
{
    boost::mutex::scoped_lock lock(mutex_);
    std::map<ContainerId, boost::shared_ptr<baidu::galaxy::container::Container> >::iterator iter =  work_containers_.begin();
    while (iter != work_containers_.end()) {
        boost::shared_ptr<baidu::galaxy::proto::ContainerInfo> ci = iter->second->ContainerInfo(fullinfo);
        cis.push_back(ci);
        iter++;
    }
}


int ContainerManager::Reload() {
    std::vector<boost::shared_ptr<baidu::galaxy::proto::ContainerMeta> > metas;
    baidu::galaxy::util::ErrorCode ec = serializer_.LoadWork(metas);
    if (ec.Code() != 0) {
        LOG(WARNING) << "load from db failed: " << ec.Message();
        return -1;
    }

    for (size_t i = 0; i < metas.size(); i++) {
        ec = res_man_->Allocate(metas[i]->container());
        ContainerId id(metas[i]->group_id(), metas[i]->container_id());
        if (ec.Code() != 0) {
            LOG(WARNING) << "allocat failed for container " << id.CompactId() 
                << " reason is:" << ec.Message();
            return -1;
        }
        boost::shared_ptr<Container> container(new Container(id, metas[i]->container()));
        if (0 != container->Reload(metas[i])) {
            LOG(WARNING) << "failed in reaload container " << id.CompactId();
            return -1;
        }
        work_containers_[id] = container;
    }
    return 0;
}

}
}
}
