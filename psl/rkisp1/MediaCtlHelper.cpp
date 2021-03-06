/*
 * Copyright (C) 2016-2017 Intel Corporation
 * Copyright (c) 2017, Fuzhou Rockchip Electronics Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "MediaCtlHelper"

#include "LogHelper.h"
#include "Camera3GFXFormat.h"
#include "MediaCtlHelper.h"
#include "MediaEntity.h"

const char* STATISTICS = "3a statistics";
const char* PARAMS = "parameters";

namespace android {
namespace camera2 {

MediaCtlHelper::MediaCtlHelper(std::shared_ptr<MediaController> mediaCtl,
        IOpenCallBack *openCallBack, bool isIMGU) :
        mOpenVideoNodeCallBack(openCallBack),
        mMediaCtl(mediaCtl),
        mMediaCtlConfig(nullptr),
        mPipeConfig(nullptr),
        mConfigedPipeType(IStreamConfigProvider::MEDIA_TYPE_MAX_COUNT)
{
    if (isIMGU)
        mMediaCtl->resetLinks();
}

MediaCtlHelper::~MediaCtlHelper()
{
    closeVideoNodes();
    resetLinks(&mConfigedMediaCtlConfigs[IStreamConfigProvider::CIO2]);
    resetLinks(&mConfigedMediaCtlConfigs[IStreamConfigProvider::IMGU_COMMON]);
}

void MediaCtlHelper::getConfigedHwPathSize(const char* pathName, uint32_t &size) {
    std::vector<MediaCtlFormatParams> &params = mConfigedMediaCtlConfigs[IStreamConfigProvider::IMGU_COMMON].mFormatParams;
    for (size_t i = 0; i < params.size(); i++) {
        if(strcmp(pathName, params[i].entityName.data()) == 0) {
            size = params[i].width * params[i].height;
            LOGI("@%s Last config : pathName:%s, size:%dx%d", __FUNCTION__, pathName, params[i].width, params[i].height);
        }
    }
}

void MediaCtlHelper::getConfigedSensorOutputSize(uint32_t &size) {
    std::vector<MediaCtlFormatParams> &params = mConfigedMediaCtlConfigs[IStreamConfigProvider::CIO2].mFormatParams;
    if(params.size() == 1)
        size = params[0].width * params[0].height;
    else
        size = 0;
    LOGI("@%s Last config: senor output size:%dx%d", __FUNCTION__,
         size == 0 ? 0 : params[0].width,
         size == 0 ? 0 : params[0].height);
}

status_t MediaCtlHelper::configure(IStreamConfigProvider &graphConfigMgr, IStreamConfigProvider::MediaType type)
{
    HAL_TRACE_CALL(CAM_GLBL_DBG_HIGH);
    status_t status = NO_ERROR;
    if (isMediaTypeForPipe(type)) {
        LOGE("%d is type for pipe!", type);
        return BAD_VALUE;
    }

    LOGI("%s: in ,type %s", __FUNCTION__, (type == IStreamConfigProvider::CIO2) ? "CIO2" : "IMGU");

    std::shared_ptr<GraphConfig> gc = graphConfigMgr.getBaseGraphConfig();
    media_device_info deviceInfo;
    CLEAR(deviceInfo);

    mConfigedPipeType = IStreamConfigProvider::MEDIA_TYPE_MAX_COUNT;
    mPipeConfig = nullptr;

    // Handle new common config
    mMediaCtlConfig = graphConfigMgr.getMediaCtlConfig(type);
    if (mMediaCtlConfig == nullptr) {
        LOGE("Not able to pick up Media Ctl configuration ");
        status = BAD_VALUE;
        return status;
    }

    mConfigedMediaCtlConfigs[type] = *mMediaCtlConfig;

    status = mMediaCtl->getMediaDevInfo(deviceInfo);
    if (status != NO_ERROR) {
        LOGE("Error getting device info");
        return status;
    }

    // setting all the Link necessary for the media controller.
    for (unsigned int i = 0; i < mMediaCtlConfig->mLinkParams.size(); i++) {
        MediaCtlLinkParams pipeLink = mMediaCtlConfig->mLinkParams[i];
        status = mMediaCtl->configureLink(pipeLink);
        if (status != NO_ERROR) {
            LOGE("Cannot set MediaCtl links (ret = %d)", status);
            return status;
        }
    }

    // open nodes after link setup
    status = openVideoNodes();
    if (status != NO_ERROR) {
        LOGE("Failed to open video nodes (ret = %d)", status);
        return status;
    }

    // HFLIP must be set before setting formats. Other controls need to be set after formats.
    for (unsigned int i = 0; i < mMediaCtlConfig->mControlParams.size(); i++) {
        MediaCtlControlParams pipeControl = mMediaCtlConfig->mControlParams[i];
        if (pipeControl.controlId == V4L2_CID_HFLIP) {
            status = mMediaCtl->setControl(pipeControl.entityName.c_str(),
                    pipeControl.controlId, pipeControl.value,
                    pipeControl.controlName.c_str());
            if (status != NO_ERROR) {
                LOGE("Cannot set HFLIP control (ret = %d)", status);
                return status;
            }
            break;
        }
    }

    // setting all the format necessary for the media controller entities
    for (unsigned int i = 0; i < mMediaCtlConfig->mFormatParams.size(); i++) {
        MediaCtlFormatParams pipeFormat = mMediaCtlConfig->mFormatParams[i];
        std::shared_ptr<MediaEntity> entity = nullptr;
        status = mMediaCtl->getMediaEntity(entity, pipeFormat.entityName.c_str());
        if (status != NO_ERROR) {
            LOGE("Getting MediaEntity \"%s\" failed", pipeFormat.entityName.c_str());
            return status;
        }
        pipeFormat.field = 0;
        //TODO: need align, zyc ?
        //if (entity->getType() == DEVICE_VIDEO)
        //    pipeFormat.stride = widthToStride(pipeFormat.formatCode, pipeFormat.width);
        //else
            pipeFormat.stride = pipeFormat.width;

        status = mMediaCtl->setFormat(pipeFormat);
        if (status != NO_ERROR) {
            LOGE("Cannot set MediaCtl format (ret = %d)", status);
            return status;
        }

        // get the capture pipe output format
        if (entity->getType() == DEVICE_VIDEO) {
            mConfigResults.pixelFormat = pipeFormat.formatCode;
            LOGI("Capture pipe output format: %s",
                    v4l2Fmt2Str(mConfigResults.pixelFormat));
        }
    }

    for (auto& it :mMediaCtlConfig->mSelectionParams) {
        //TODO, only isp subdev can set selection params now ,zyc
        if (it.entityName.find("isp-subdev") == string::npos)
            continue;
        status = mMediaCtl->setSelection(it.entityName.c_str(), it.pad, it.target, it.top, it.left, it.width, it.height);
        if (status != NO_ERROR) {
            LOGE("Cannot set subdev MediaCtl format selection (ret = %d)", status);
            return status;
        }
     }

    // setting all the selections
    for (auto& it :mMediaCtlConfig->mSelectionVideoParams) {
        std::shared_ptr<MediaEntity> entity;
        std::shared_ptr<V4L2VideoNode> vNode;

        status = mMediaCtl->getMediaEntity(entity, it.entityName.c_str());
        if (status != NO_ERROR) {
            LOGE("Cannot get media entity (ret = %d)", status);
            return status;
        }
        status = entity->getDevice((std::shared_ptr<V4L2DeviceBase>&)vNode);
        if (status != NO_ERROR) {
            LOGE("Cannot get media entity device (ret = %d)", status);
            return status;
        }

        status = vNode->setSelection(it.select);
        if (status != NO_ERROR) {
            LOGE("Cannot set vnode MediaCtl format selection (ret = %d)", status);
            return status;
        }
     }

    // setting all the basic controls necessary for the media controller entities.
    // HFLIP already set earlier, so no need to set it again.
    for (unsigned int i = 0; i < mMediaCtlConfig->mControlParams.size(); i++) {
        MediaCtlControlParams pipeControl = mMediaCtlConfig->mControlParams[i];
        if (pipeControl.controlId != V4L2_CID_HFLIP) {
            status = mMediaCtl->setControl(pipeControl.entityName.c_str(),
                    pipeControl.controlId, pipeControl.value,
                    pipeControl.controlName.c_str());
            if (status != NO_ERROR) {
                LOGE("Cannot set MediaCtl control (ret = %d)", status);
                return status;
            }
        }
    }

    return status;
}

status_t MediaCtlHelper::configurePipe(IStreamConfigProvider &graphConfigMgr,
                                       IStreamConfigProvider::MediaType pipeType,
                                       bool resetFormat)
{
    LOGI("%s: %d ->%d", __FUNCTION__, mConfigedPipeType, pipeType);
    status_t status = OK;
    if (!isMediaTypeForPipe(pipeType)) {
        LOGE("%d is not type for pipe!", pipeType);
        return BAD_VALUE;
    }

    if (mConfigedPipeType == pipeType)
        return OK;

    // Disable link for the old MediaCtlData
    const MediaCtlConfig* config = graphConfigMgr.getMediaCtlConfig(mConfigedPipeType);
    if (config) {
        for (size_t i = 0; i < config->mLinkParams.size(); i++) {
            MediaCtlLinkParams pipeLink = config->mLinkParams[i];
            pipeLink.enable = false;
            status = mMediaCtl->configureLink(pipeLink);
            if (status != NO_ERROR) {
                LOGE("Cannot set MediaCtl links (ret = %d)", status);
                return status;
            }
        }
    }

    // Config for the new MediaCtlData
    config = graphConfigMgr.getMediaCtlConfig(pipeType);
    if (!config) {
        return OK;
    }

    mPipeConfig = config; // Remeber it for disabling link in ~MediaCtlHelper()
    mConfigedPipeType = pipeType;
    for (size_t i = 0; i < config->mLinkParams.size(); i++) {
        MediaCtlLinkParams pipeLink = config->mLinkParams[i];
        status = mMediaCtl->configureLink(pipeLink);
        if (status != NO_ERROR) {
            LOGE("Cannot set MediaCtl links (ret = %d)", status);
            return status;
        }
    }
    if (!resetFormat)
        return OK;

    for (size_t i = 0; i < config->mFormatParams.size(); i++) {
        MediaCtlFormatParams pipeFormat = config->mFormatParams[i];
        pipeFormat.field = 0;
        //pipeFormat.stride = widthToStride(pipeFormat.formatCode, pipeFormat.width);
        pipeFormat.stride = pipeFormat.width;

        status = mMediaCtl->setFormat(pipeFormat);
        if (status != NO_ERROR) {
            LOGE("Cannot set MediaCtl format (ret = %d)", status);
            return status;
        }
    }
    return OK;
}

status_t MediaCtlHelper::openVideoNodes()
{
    LOGD("@%s", __FUNCTION__);
    status_t status = UNKNOWN_ERROR;

    mConfiguredNodes.clear();
    mConfiguredNodesPerName.clear();

    // Open video nodes that are listed in the current config
    for (unsigned int i = 0; i < mMediaCtlConfig->mVideoNodes.size(); i++) {
        MediaCtlElement element = mMediaCtlConfig->mVideoNodes[i];
        NodeTypes isysNodeName = (NodeTypes) element.isysNodeName;
        status = openVideoNode(element.name.c_str(), isysNodeName);
        if (status != NO_ERROR) {
            LOGE("Cannot open video node (status = 0x%X)", status);
            return status;
        }
    }

    return NO_ERROR;
}

status_t MediaCtlHelper::openVideoNode(const char *entityName, NodeTypes isysNodeName)
{
    LOGI("@%s: %s, node: %d", __FUNCTION__, entityName, isysNodeName);
    status_t status = UNKNOWN_ERROR;
    std::shared_ptr<MediaEntity> entity = nullptr;
    std::shared_ptr<V4L2VideoNode> videoNode = nullptr;

    if (entityName != nullptr) {
        status = mMediaCtl->getMediaEntity(entity, entityName);
        if (status != NO_ERROR) {
            LOGE("Getting MediaEntity \"%s\" failed", entityName);
            return status;
        }

        status = entity->getDevice((std::shared_ptr<V4L2DeviceBase>&) videoNode);
        if (status != NO_ERROR) {
            LOGE("Error opening device \"%s\"", entityName);
            return status;
        }

        mConfiguredNodes.push_back(videoNode);
        // mConfiguredNodesPerName is sorted from lowest to highest NodeTypes value
        mConfiguredNodesPerName.insert(std::make_pair(isysNodeName, videoNode));
        if (mOpenVideoNodeCallBack) {
            status = mOpenVideoNodeCallBack->opened(isysNodeName, videoNode);
        }
    }

    return status;
}

status_t MediaCtlHelper::closeVideoNodes()
{
    LOGD("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    for (size_t i = 0; i < mConfiguredNodes.size(); i++) {
        status = mConfiguredNodes[i]->close();
        if (status != NO_ERROR)
            LOGW("Error in closing video node (%zu)", i);
    }
    mConfiguredNodes.clear();
    mConfiguredNodesPerName.clear();

    return NO_ERROR;
}


status_t MediaCtlHelper::resetLinks(const MediaCtlConfig *config)
{
    LOGD("@%s", __FUNCTION__);
    status_t status = NO_ERROR;

    if (config == nullptr) {
        LOGW("%s mMediaCtlConfig is NULL", __FUNCTION__);
        return status;
    }

    for (size_t i = 0; i < config->mLinkParams.size(); i++) {
        MediaCtlLinkParams pipeLink = config->mLinkParams[i];
        pipeLink.enable = false;
        status = mMediaCtl->configureLink(pipeLink);

        if (status != NO_ERROR) {
            LOGE("Cannot reset MediaCtl link (ret = %d)", status);
            return status;
        }
    }

    return status;
}


} /* namespace camera2 */
} /* namespace android */
