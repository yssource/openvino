// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "transpose.h"
#include "ie_parallel.hpp"

#include <algorithm>
#include <string>
#include <dnnl_extension_utils.h>
#include <common/primitive_hashing_utils.hpp>

using namespace mkldnn;
using namespace InferenceEngine;

namespace ov {
namespace intel_cpu {
namespace node {
namespace {
struct TransposeAsReorderKey {
    mkldnn::memory::desc src;
    mkldnn::memory::desc dest;
    size_t hash() const;
    bool operator==(const TransposeAsReorderKey& rhs) const;
};

size_t TransposeAsReorderKey::hash() const {
    using namespace dnnl::impl;
    using namespace dnnl::impl::primitive_hashing;

    size_t seed = 0;
    seed = hash_combine(seed, get_md_hash(src.data));
    seed = hash_combine(seed, get_md_hash(dest.data));

    return seed;
}

bool TransposeAsReorderKey::operator==(const TransposeAsReorderKey& rhs) const {
    bool retVal = true;
    retVal = src == rhs.src && dest == rhs.dest;
    return retVal;
}
}  // namespace

bool Transpose::isSupportedOperation(const std::shared_ptr<const ov::Node>& op, std::string& errorMessage) noexcept {
    try {
        if (!one_of(op->get_type_info(),
                ov::op::v1::Transpose::get_type_info_static())) {
            errorMessage = "Node is not an instance of the Transpose operation from opset1.";
            return false;
        }

        if (op->get_input_node_ptr(INPUT_ORDER_IDX)->get_type_info() != ov::op::v0::Constant::get_type_info_static()) {
            // TODO: Support parameterized Order input for dynamic shapes.
            errorMessage = "Constant expected as the second input for static shapes.";
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

Transpose::Transpose(const std::shared_ptr<ov::Node>& op, const mkldnn::engine& eng, WeightsSharing::Ptr &cache)
        : Node(op, eng, cache) {
    std::string errorMessage;
    if (!isSupportedOperation(op, errorMessage)) {
        IE_THROW(NotImplemented) << errorMessage;
    }

    if (op->get_input_node_ptr(INPUT_ORDER_IDX)->get_type_info() == ov::op::v0::Constant::get_type_info_static()) {
        isInputOrderConst = true;
        order = ov::as_type<ov::op::v0::Constant>(op->get_input_node_ptr(INPUT_ORDER_IDX))->cast_vector<size_t>();

        if (order.empty()) {
            size_t rank = getInputShapeAtPort(INPUT_DATA_IDX).getRank();
            for (size_t i = 1lu; i <= rank; ++i) {
                order.emplace_back(rank - i);
            }
        }
    }
}

void Transpose::getSupportedDescriptors() {
}

void Transpose::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty())
        return;

    prec = getOriginalInputPrecisionAtPort(0);

    auto& creatorsMap = BlockedDescCreator::getCommonCreators();

    NodeConfig config;
    config.dynBatchSupport = true;
    config.inConfs.resize(2);
    config.outConfs.resize(1);
    config.inConfs[INPUT_DATA_IDX].inPlace(-1);
    config.inConfs[INPUT_DATA_IDX].constant(false);
    config.inConfs[INPUT_ORDER_IDX].constant(isInputOrderConst);
    config.inConfs[INPUT_ORDER_IDX].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(
            Precision::I32, getInputShapeAtPort(INPUT_ORDER_IDX)));
    config.outConfs[0].inPlace(-1);
    config.outConfs[0].constant(false);

    const auto& inputDataShape = getInputShapeAtPort(INPUT_DATA_IDX);
    const auto& outputDataShape = getOutputShapeAtPort(0);
    if (inputDataShape.getRank() == 4 || inputDataShape.getRank() == 5) {
        config.inConfs[0].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(prec, inputDataShape));
        config.outConfs[0].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(prec, outputDataShape));
        supportedPrimitiveDescriptors.push_back({config, impl_desc_type::unknown});

        const auto& srcDims = inputDataShape.getDims();
        if (srcDims[1] != Shape::UNDEFINED_DIM && srcDims[1] % 8 == 0) {
            config.inConfs[0].setMemDesc(creatorsMap.at(LayoutType::nCsp8c)->createSharedDesc(prec, inputDataShape));
            supportedPrimitiveDescriptors.push_back({config, impl_desc_type::unknown});
        }

        if (srcDims[1] != Shape::UNDEFINED_DIM && srcDims[1] % 16 == 0) {
            config.inConfs[0].setMemDesc(creatorsMap.at(LayoutType::nCsp16c)->createSharedDesc(prec, inputDataShape));
            supportedPrimitiveDescriptors.push_back({config, impl_desc_type::unknown});
        }

        if (prec == Precision::FP32 || prec == Precision::I8 || prec == Precision::U8) {
            config.inConfs[0].setMemDesc(creatorsMap.at(LayoutType::nspc)->createSharedDesc(prec, inputDataShape));
            config.outConfs[0].setMemDesc(creatorsMap.at(LayoutType::nspc)->createSharedDesc(prec, outputDataShape));
            supportedPrimitiveDescriptors.push_back({config, impl_desc_type::unknown});
        }
    } else {
        // general plain case
        config.inConfs[0].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(prec, inputDataShape));
        config.outConfs[0].setMemDesc(creatorsMap.at(LayoutType::ncsp)->createSharedDesc(prec, outputDataShape));
        supportedPrimitiveDescriptors.push_back({config, impl_desc_type::unknown});
    }
}

bool Transpose::isExecutable() const {
    return !isInputTensorAtPortEmpty(0);
}

bool Transpose::needPrepareParams() const {
    if (isOptimized)
        return false;
    return inputShapesModified();
}

void Transpose::prepareParams() {
    auto srcDesc = getParentEdgeAt(INPUT_DATA_IDX)->getMemory().GetDescWithType<BlockedMemoryDesc>();
    params.src_block_dims = srcDesc->getBlockDims();
    auto dstDesc = getChildEdgeAt(0)->getMemory().GetDescWithType<BlockedMemoryDesc>();
    params.dst_block_dims = dstDesc->getBlockDims();

    if (performAsReorder) {
        mkldnn::primitive_attr attr;
        const auto engine = getEngine();
        auto& dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
        auto& srcMemPtr = getParentEdgeAt(INPUT_DATA_IDX)->getMemoryPtr();
        MemoryPtr src_blocked = std::make_shared<Memory>(engine);
        MemoryPtr dst_blocked = std::make_shared<Memory>(engine);

        dst_blocked->Create(
            DnnlExtensionUtils::makeDescriptor(dstMemPtr->GetDescWithType<DnnlMemoryDesc>()->getDnnlDesc()),
            dstMemPtr->GetData(), false);

        const auto newDims = dst_blocked->getStaticDims();
        auto newDesc = mkldnn::memory::desc(DnnlExtensionUtils::convertToDnnlDims(newDims),
                                            dst_blocked->GetDataType(),
                                            memory::format_tag::acdb);
        src_blocked->Create(DnnlExtensionUtils::makeDescriptor(newDesc), srcMemPtr->GetData(), false);

        impl_desc_type impl_type = getSelectedPrimitiveDescriptor()->getImplementationType();
        TransposeAsReorderKey key = {src_blocked->GetPrimitive().get_desc(), dst_blocked->GetPrimitive().get_desc()};
        auto builder = [&engine, &impl_type](const TransposeAsReorderKey& key) -> std::shared_ptr<mkldnn::primitive> {
            mkldnn::primitive_attr attr;
            reorder::primitive_desc pd = mkldnn::reorder::primitive_desc(engine, key.src, engine, key.dest, attr, true);

            if (!pd)
                return nullptr;
            auto info = pd.impl_info_str();
            impl_type = parse_impl_name(info);
            return std::make_shared<mkldnn::reorder>(pd);
        };

        auto cache = getRuntimeCache();
        auto result = cache->getOrCreate(key, builder);

        if (!result.first) {
            IE_THROW() << "Reorder primitive descriptor was not found for Transpose node " << getName() << ".";
        }

        prim = result.first;

        supportedPrimitiveDescriptors[0].setImplementationType(impl_type);
        primArgs = {{DNNL_ARG_SRC, getParentEdgesAtPort(INPUT_DATA_IDX)[0]->getMemoryPtr()->GetPrimitive()},
                    {DNNL_ARG_DST, getChildEdgesAtPort(0)[0]->getMemoryPtr()->GetPrimitive()}};
        return;
    }

    if (!isInputOrderConst) {
        auto orderPtr = reinterpret_cast<const int32_t*>(getParentEdgeAt(0)->getMemoryPtr()->GetPtr());
        auto orderLen = getParentEdgeAt(0)->getMemoryPtr()->GetSize();
        params.order.assign(orderPtr, orderPtr + orderLen);
    }

    auto engine = getEngine();
    auto builder = [&engine](const PermuteParams& key) -> std::shared_ptr<TransposeJitExecutor> {
        return std::make_shared<TransposeJitExecutor>(key);
    };

    auto cache = getRuntimeCache();
    auto result = cache->getOrCreate(params, builder);

    if (!result.first) {
        IE_THROW() << "Primitive descriptor was not found for node " << getName() << ".";
    }

    execPtr = result.first;
}

void Transpose::createPrimitive() {
    auto& dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
    auto& srcMemPtr = getParentEdgeAt(INPUT_DATA_IDX)->getMemoryPtr();
    if (!dstMemPtr || !dstMemPtr->isAllocated())
        IE_THROW() << "Destination memory was not allocated.";
    if (!srcMemPtr || !srcMemPtr->isAllocated())
        IE_THROW() << "Input memory was not allocated.";
    if (getSelectedPrimitiveDescriptor() == nullptr)
        IE_THROW() << "Preferable primitive descriptor was not set.";

    if (getParentEdgeAt(INPUT_DATA_IDX)->getMemory().getDesc().hasLayoutType(LayoutType::ncsp) &&
        getChildEdgeAt(0)->getMemory().getDesc().hasLayoutType(LayoutType::ncsp) &&
        order == std::vector<size_t>{0, 3, 1, 2}) {
        performAsReorder = true;
    } else if (getParentEdgeAt(INPUT_DATA_IDX)->getMemory().getDesc().hasLayoutType(LayoutType::ncsp) &&
            std::find(optimizedOrders.begin(), optimizedOrders.end(), order) != optimizedOrders.end()) {
        isOptimized = true;
        execPtr = std::make_shared<TransposeRefExecutor>();
        return;
    }

    params.data_size = getSelectedPrimitiveDescriptor()->getConfig().inConfs[0].getMemDesc()->getPrecision().size();
    if (isInputOrderConst)
        params.order = order;
    auto srcDesc = getParentEdgeAt(INPUT_DATA_IDX)->getMemory().GetDescWithType<BlockedMemoryDesc>();
    params.src_block_order = srcDesc->getOrder();
    auto dstDesc = getChildEdgeAt(0)->getMemory().GetDescWithType<BlockedMemoryDesc>();
    params.dst_block_order = dstDesc->getOrder();

    if (inputShapesDefined() && isExecutable()) {
        prepareParams();
        updateLastInputDims();
    }
}

template <typename T>
static void transpose_to_0312(const int MB, const MemoryPtr& srcMemPtr, MemoryPtr& dstMemPtr) {
    const auto src_data = reinterpret_cast<const T*>(srcMemPtr->GetPtr());
    auto dst_data = reinterpret_cast<T*>(dstMemPtr->GetPtr());

    const int DIM1 = srcMemPtr->getStaticDims()[1];
    const int DIM2 = srcMemPtr->getStaticDims()[2];
    const int DIM3 = srcMemPtr->getStaticDims()[3];

    parallel_for3d(MB, DIM1, DIM2, [&](const int n, const int dim1, const int dim2) {
        for (int dim3 = 0; dim3 < DIM3; ++dim3) {
            const int src_off = n * DIM1 * DIM2 * DIM3 +
                                dim1 * DIM2 * DIM3 +
                                dim2 * DIM3 +
                                dim3;
            const int dst_off = n * DIM1 * DIM2 * DIM3 +
                                dim3 * DIM1 * DIM2 +
                                dim1 * DIM2 +
                                dim2;

            dst_data[dst_off] = src_data[src_off];
        }
    });
}

template<typename T>
static void transpose_to_04123(const int MB, const MemoryPtr& srcMemPtr, MemoryPtr& dstMemPtr) {
    const auto src_data = reinterpret_cast<const T*>(srcMemPtr->GetPtr());
    auto dst_data = reinterpret_cast<T*>(dstMemPtr->GetPtr());

    const int DIM1 = srcMemPtr->getStaticDims()[1];
    const int DIM2 = srcMemPtr->getStaticDims()[2];
    const int DIM3 = srcMemPtr->getStaticDims()[3];
    const int DIM4 = srcMemPtr->getStaticDims()[4];

    parallel_for4d(MB, DIM1, DIM2, DIM3, [&](const int n, const int dim1, const int dim2, const int dim3) {
        for (int dim4 = 0; dim4 < DIM4; ++dim4) {
            const int src_off = n * DIM1 * DIM2 * DIM3 * DIM4 +
                                dim1 * DIM2 * DIM3 * DIM4 +
                                dim2 * DIM3 * DIM4 +
                                dim3 * DIM4 +
                                dim4;
            const int dst_off = n * DIM1 * DIM2 * DIM3 * DIM4 +
                                dim4 * DIM1 * DIM2 * DIM3 +
                                dim1 * DIM2 * DIM3 +
                                dim2 * DIM3 +
                                dim3;

            dst_data[dst_off] = src_data[src_off];
        }
    });
}

template<typename T>
static void transpose_to_051234(const int MB, const MemoryPtr& srcMemPtr, MemoryPtr& dstMemPtr) {
    const auto src_data = reinterpret_cast<const T*>(srcMemPtr->GetPtr());
    auto dst_data = reinterpret_cast<T*>(dstMemPtr->GetPtr());

    const int DIM1 = srcMemPtr->getStaticDims()[1];
    const int DIM2 = srcMemPtr->getStaticDims()[2];
    const int DIM3 = srcMemPtr->getStaticDims()[3];
    const int DIM4 = srcMemPtr->getStaticDims()[4];
    const int DIM5 = srcMemPtr->getStaticDims()[5];

    parallel_for5d(MB, DIM1, DIM2, DIM3, DIM4, [&](const int n, const int dim1, const int dim2, const int dim3, const int dim4) {
        for (int dim5 = 0; dim5 < DIM5; ++dim5) {
            const int src_off = n * DIM1 * DIM2 * DIM3 * DIM4 * DIM5 +
                                dim1 * DIM2 * DIM3 * DIM4 * DIM5 +
                                dim2 * DIM3 * DIM4 * DIM5 +
                                dim3 * DIM4 * DIM5 +
                                dim4 * DIM5 +
                                dim5;
            const int dst_off = n * DIM5 * DIM1 * DIM2 * DIM3 * DIM4 +
                                dim5 * DIM1 * DIM2 * DIM3 * DIM4 +
                                dim1 * DIM2 * DIM3 * DIM4 +
                                dim2 * DIM3 * DIM4 +
                                dim3 * DIM4 +
                                dim4;

            dst_data[dst_off] = src_data[src_off];
        }
    });
}

template<typename T>
void Transpose::optimizedExecute(const int MB, const MemoryPtr& srcMemPtr, MemoryPtr& dstMemPtr) {
    switch (srcMemPtr->getStaticDims().size()) {
        case 4:
            transpose_to_0312<T>(MB, srcMemPtr, dstMemPtr);
            break;
        case 5:
            transpose_to_04123<T>(MB, srcMemPtr, dstMemPtr);
            break;
        case 6:
            transpose_to_051234<T>(MB, srcMemPtr, dstMemPtr);
            break;
        default:
            IE_THROW() << "Transpose '" << getName() << "' supports optimized execution with only 4D, 5D and 6D shapes";
    }
}

void Transpose::execute(mkldnn::stream strm) {
    if (prim) {
        (*prim).execute(strm, primArgs);
    } else if (execPtr) {
        auto &dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
        auto &srcMemPtr = getParentEdgeAt(INPUT_DATA_IDX)->getMemoryPtr();

        int MB = 0;
        if (isDynamicNode()) {
            MB = srcMemPtr->getStaticDims()[0];
        } else {
            MB = batchToProcess();
        }

        execPtr->exec(this, srcMemPtr, dstMemPtr, MB);
    } else {
        IE_THROW() << "Could not execute Transpose node. Primitive was not created.";
    }
}

void Transpose::executeDynamicImpl(mkldnn::stream strm) {
    execute(strm);
}

Transpose::TransposeJitExecutor::TransposeJitExecutor(const PermuteParams& params) {
    pKernel = std::make_shared<PermuteKernel>(params);
}

void Transpose::TransposeJitExecutor::exec(Transpose* node, MemoryPtr& srcMemPtr, MemoryPtr& dstMemPtr, const int MB) {
    if (!pKernel)
        IE_THROW() << "Could not execute. Kernel for Transpose node was not compiled.";

    const uint8_t* srcData = reinterpret_cast<const uint8_t*>(srcMemPtr->GetPtr());
    uint8_t* dstData = reinterpret_cast<uint8_t*>(dstMemPtr->GetPtr());

    pKernel->execute(srcData, dstData, MB);
}

void Transpose::TransposeRefExecutor::exec(Transpose* node, MemoryPtr& srcMemPtr, MemoryPtr& dstMemPtr, const int MB) {
    const size_t dataSize = srcMemPtr->getDesc().getPrecision().size();
    TransposeContext ctx = {node, srcMemPtr, dstMemPtr, MB};
    OV_SWITCH(intel_cpu, TransposeOptimizedEmitter, ctx, dataSize,
              OV_CASE(1, PrecisionTrait<Precision::U8>::value_type),
              OV_CASE(2, PrecisionTrait<Precision::U16>::value_type),
              OV_CASE(4, PrecisionTrait<Precision::I32>::value_type));
}

bool Transpose::created() const {
    return getType() == Type::Transpose;
}

}   // namespace node
}   // namespace intel_cpu
}   // namespace ov
