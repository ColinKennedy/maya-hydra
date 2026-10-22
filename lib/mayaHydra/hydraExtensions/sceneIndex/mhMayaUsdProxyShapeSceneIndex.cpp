//
// Copyright 2023 Autodesk
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "mhMayaUsdProxyShapeSceneIndex.h"

#include <mayaHydraLib/pick/mhUsdPickHandler.h>
#include <mayaHydraLib/pick/mhPickHandlerRegistry.h>

#include <flowViewport/fvpInstruments.h>

#include <pxr/imaging/hd/extentSchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/usd/usdGeom/modelAPI.h>

#include <maya/MGlobal.h>

//mayaHydra headers
#include "ufeExtensions/Global.h"

PXR_NAMESPACE_USING_DIRECTIVE

namespace MAYAHYDRA_NS_DEF {

MayaUsdProxyShapeSceneIndex::MayaUsdProxyShapeSceneIndex(
    const MAYAUSDAPI_NS::ProxyStage&       proxyStage,
    const HdSceneIndexBaseRefPtr&          sceneIndexChainLastElement,
    const UsdImagingStageSceneIndexRefPtr& usdImagingStageSceneIndex,
    const MObjectHandle&                   dagNodeHandle,
    const PXR_NS::SdfPath&                 prefix,
    const std::shared_ptr<UsdStageRefPtr>& sharedStage,
    const Fvp::BboxSceneIndexRefPtr&       extentsHintSceneIndex)
    : ParentClass(sceneIndexChainLastElement)
    , InputSceneIndexUtils(sceneIndexChainLastElement)
    , _usdImagingStageSceneIndex(usdImagingStageSceneIndex)
    , _proxyStage(proxyStage)
    , _sharedStage(sharedStage)
    , _extentsHintSceneIndex(extentsHintSceneIndex)
    , _dagNodeHandle(dagNodeHandle)
    , _prefix(prefix)
{
    TfWeakPtr<MayaUsdProxyShapeSceneIndex> ptr(this);
    _stageSetNoticeKey = TfNotice::Register(ptr, &MayaUsdProxyShapeSceneIndex::_StageSet);
    _stageInvalidateNoticeKey = TfNotice::Register(ptr, &MayaUsdProxyShapeSceneIndex::_StageInvalidate);
    _objectsChangedNoticeKey = TfNotice::Register(ptr, &MayaUsdProxyShapeSceneIndex::_ObjectsChanged);

    Fvp::Instruments::instance().set(kNbPopulateCalls, VtValue(_nbPopulateCalls));

    // Add our pick handler to the pick handler registry.  All USD scene indices
    // could share the same pick handler, but create a new one for simplicity.
    auto pickHandler = std::make_shared<UsdPickHandler>();
    TF_AXIOM(PickHandlerRegistry::Instance().Register(prefix, pickHandler));
}

MayaUsdProxyShapeSceneIndex::~MayaUsdProxyShapeSceneIndex()
{
    TfNotice::Revoke(_stageSetNoticeKey);
    TfNotice::Revoke(_stageInvalidateNoticeKey);
    TfNotice::Revoke(_objectsChangedNoticeKey);

    TF_AXIOM(PickHandlerRegistry::Instance().Unregister(_prefix));
}

MayaUsdProxyShapeSceneIndexRefPtr MayaUsdProxyShapeSceneIndex::New(
    const MAYAUSDAPI_NS::ProxyStage&       proxyStage, 
    const HdSceneIndexBaseRefPtr&          sceneIndexChainLastElement,
    const UsdImagingStageSceneIndexRefPtr& usdImagingStageSceneIndex,
    const MObjectHandle&                   dagNodeHandle,
    const PXR_NS::SdfPath&                 prefix,
    const std::shared_ptr<UsdStageRefPtr>& sharedStage,
    const Fvp::BboxSceneIndexRefPtr&       extentsHintSceneIndex)
{
    return TfCreateRefPtr(new MayaUsdProxyShapeSceneIndex(
        proxyStage, sceneIndexChainLastElement, usdImagingStageSceneIndex,
        dagNodeHandle, prefix, sharedStage, extentsHintSceneIndex));
}

void MayaUsdProxyShapeSceneIndex::UpdateTime()
{
    if (_usdImagingStageSceneIndex && _dagNodeHandle.isValid()) {
        _usdImagingStageSceneIndex->SetTime(_proxyStage.getTime());//We have the possibility to scale and offset the time in _proxyShapeBase
    }
}

bool MayaUsdProxyShapeSceneIndex::IsModelWithAuthoredExtentsHint(
    const SdfPath& primPath) const
{
    if (!_stage) {
        return false;
    }

    const UsdPrim prim = _stage->GetPrimAtPath(primPath);
    return prim && prim.IsModel()
        && UsdGeomModelAPI(prim).GetExtentsHintAttr().HasAuthoredValueOpinion()
        && (!prim.HasPayload() || !prim.IsLoaded());
}

void MayaUsdProxyShapeSceneIndex::_StageSet(const MAYAUSDAPI_NS::ProxyStageSetNotice& notice) 
{ 
    _populated = false;
    Populate(); 
}

// Stage invalidated.  See
// https://github.com/Autodesk/maya-usd/blob/dev/lib/mayaUsd/nodes/proxyShapeBase.cpp    
// for all inputs that can invalidate the stage, among which:
// - the USD file path
// - the USD prim at the root of the stage
// - the input stage cache ID, e.g. for a Bifrost-generated stage.  Note that
//   in this case, the mayaUsd stage pointer DOES NOT CHANGE: the 
//   Bifrost-generated stage is added as a sub-layer of the mayaUsd stage.
// - etc.
// In these cases we set the stage to null and start over.
void MayaUsdProxyShapeSceneIndex::_StageInvalidate(const MAYAUSDAPI_NS::ProxyStageInvalidateNotice& notice) 
{ 
    _stage.Reset();
    if (_sharedStage) {
        _sharedStage->Reset();
    }
    if (_extentsHintSceneIndex) {
        _extentsHintSceneIndex->refreshSyntheticPrims();
    }
    _usdImagingStageSceneIndex->SetStage(nullptr);
    _populated = false;

    // MayaUSD sends this notice from setDependentsDirty(), before dirty
    // propagation completes and outStageData can compute the replacement
    // stage. Defer acquisition until Maya reaches its idle boundary.
    if (!_populateOnIdlePending.exchange(true)) {
        auto* self = new MayaUsdProxyShapeSceneIndexRefPtr(this);
        const MStatus status = MGlobal::executeTaskOnIdle(
            &_PopulateOnIdle, self, MGlobal::kHighIdlePriority);
        if (!status) {
            _populateOnIdlePending = false;
            delete self;
        }
    }
}

void MayaUsdProxyShapeSceneIndex::_PopulateOnIdle(void* clientData)
{
    std::unique_ptr<MayaUsdProxyShapeSceneIndexRefPtr> self(
        static_cast<MayaUsdProxyShapeSceneIndexRefPtr*>(clientData));
    MayaUsdProxyShapeSceneIndexRefPtr sceneIndex = *self;
    sceneIndex->_populateOnIdlePending = false;
    if (sceneIndex->_dagNodeHandle.isValid() && sceneIndex->_dagNodeHandle.isAlive()) {
        sceneIndex->Populate();
    }
}

void MayaUsdProxyShapeSceneIndex::_ObjectsChanged(
    const MAYAUSDAPI_NS::ProxyStageObjectsChangedNotice& notice)
{
    _PopulateAndApplyPendingChanges();
}

void MayaUsdProxyShapeSceneIndex::_PopulateAndApplyPendingChanges() 
{ 
    Populate();
    _usdImagingStageSceneIndex->ApplyPendingUpdates();
    if (_extentsHintSceneIndex) {
        _extentsHintSceneIndex->refreshSyntheticPrims();
    }
}

void MayaUsdProxyShapeSceneIndex::Populate()
{
    if (!_populated) {
        auto stage = _proxyStage.getUsdStage();
        bool hasAuthoredRootPrims = false;
        if (stage) {
            const auto rootPrims = stage->GetRootLayer()->GetRootPrims();
            hasAuthoredRootPrims = rootPrims.begin() != rootPrims.end();
        }
        if (hasAuthoredRootPrims) {
            ++_nbPopulateCalls;
            Fvp::Instruments::instance().set(kNbPopulateCalls, VtValue(_nbPopulateCalls));
            _stage = stage;
            if (_sharedStage) {
                *_sharedStage = stage;
            }
            _usdImagingStageSceneIndex->SetStage(stage);
            if (_extentsHintSceneIndex) {
                _extentsHintSceneIndex->refreshSyntheticPrims();
            }
            // Set the initial time
            UpdateTime();
            _populated = true;
        }
    }
}

Ufe::Path MayaUsdProxyShapeSceneIndex::InterpretRprimPath(
    const HdSceneIndexBaseRefPtr& sceneIndex,
    const SdfPath&                path)
{
    if (MayaUsdProxyShapeSceneIndexRefPtr proxyShapeSceneIndex = TfDynamic_cast<MayaUsdProxyShapeSceneIndexRefPtr>(sceneIndex)) {
        MDagPath dagPath(MDagPath::getAPathTo(proxyShapeSceneIndex->_dagNodeHandle.object()));
        return Ufe::Path(
            { UfeExtensions::dagPathToUfePathSegment(dagPath), UfeExtensions::sdfPathToUfePathSegment(path,  UfeExtensions::getUsdRunTimeId()) });
    }

    return Ufe::Path();
}

// satisfying HdSceneIndexBase
HdSceneIndexPrim MayaUsdProxyShapeSceneIndex::GetPrim(const SdfPath& primPath) const
{
    HdSceneIndexPrim prim = GetInputSceneIndex()->GetPrim(primPath);
    if (!prim.dataSource || !prim.primType.IsEmpty()
        || !IsModelWithAuthoredExtentsHint(primPath)) {
        return prim;
    }

    HdExtentSchema existingExtent
        = HdExtentSchema::GetFromParent(prim.dataSource);
    if (existingExtent.GetMin() && existingExtent.GetMax()) {
        return prim;
    }

    VtVec3fArray extentsHint;
    const UsdPrim primOnStage = _stage->GetPrimAtPath(primPath);
    if (!UsdGeomModelAPI(primOnStage).GetExtentsHint(&extentsHint)
        || extentsHint.size() < 2) {
        return prim;
    }

    // The first pair is the default-purpose range.  This is the same range
    // usdview uses for the model's unloaded bounds.
    const HdContainerDataSourceHandle extentOverlay
        = HdRetainedContainerDataSource::New(
            HdExtentSchemaTokens->extent,
            HdExtentSchema::Builder()
                .SetMin(HdRetainedTypedSampledDataSource<GfVec3d>::New(
                    GfVec3d(extentsHint[0])))
                .SetMax(HdRetainedTypedSampledDataSource<GfVec3d>::New(
                    GfVec3d(extentsHint[1])))
                .Build());
    prim.dataSource = HdOverlayContainerDataSource::New(
        extentOverlay, prim.dataSource);
    return prim;
}

SdfPathVector MayaUsdProxyShapeSceneIndex::GetChildPrimPaths(const SdfPath& primPath) const
{
    return GetInputSceneIndex()->GetChildPrimPaths(primPath);
}

} // namespace MAYAHYDRA_NS_DEF
