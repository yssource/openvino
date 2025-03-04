# OpenVINO™ Transition Guide for API 2.0 {#openvino_2_0_transition_guide}

@sphinxdirective

.. toctree::
   :maxdepth: 1
   :hidden:

   openvino_2_0_deployment
   openvino_2_0_inference_pipeline
   openvino_2_0_configure_devices
   openvino_2_0_preprocessing
   openvino_2_0_model_creation

@endsphinxdirective

### Introduction

Older versions of OpenVINO™ (prior to 2022.1) required to change the logic of applications when an user migrates from the frameworks like TensorFlow, ONNX Runtime, PyTorch, PaddlePaddle, etc. The change of application's logic is connected with:

- Model Optimizer changed input precisions for some inputs. For example, neural language processing models with `I64` input are becoming to have `I32` input element type.
- Model Optimizer changed layouts for TensorFlow models (see [Layouts in OpenVINO](../layout_overview.md)). It leads to unexpected user behavior that a user needs to use a different layout for its input data with compare to the framework:
![tf_openvino]
- Inference Engine API (`InferenceEngine::CNNNetwork`) also applied some conversion rules for input and output precisions because of device plugins limitations.
- Users need to specify input shapes during model conversions in Model Optimizer and work with static shapes in the application.

OpenVINO™ introduces API 2.0 (also called OpenVINO API v2) to align the logic of working with model as it is done in the frameworks - no layout and precision changes, operates with tensor names and indices to address inputs and outputs. OpenVINO Runtime is composed of Inference Engine API used for inference and nGraph API targeted to work with models and operations. API 2.0 has common structure, naming convention styles, namespaces, and removes duplicated structures. See [Changes to Inference Pipeline in OpenVINO API v2](common_inference_pipeline.md) for details.

> **NOTE**: Most important is that your existing application can continue working with OpenVINO Runtime 2022.1 as it used to be, but we recommend migration to API 2.0 to unlock additional features like [Preprocessing](../preprocessing_overview.md) and [Dynamic shapes support](../ov_dynamic_shapes.md).

### Introducing IR v11

To support these features, OpenVINO introduced IR v11 which is generated by Model Optimizer by default since 2022.1. The model represented in IR v11 fully matches the original model in a original framework format in terms of inputs and outputs. Also, a user does not have to specify input shapes during the conversion, so the resulting IR v11 contains `-1` to denote undefined dimensions (see [Working with dynamic shapes](../ov_dynamic_shapes.md) to fully utilize this feature; or [Changing input shapes](../ShapeInference.md) to reshape to static shapes in the application).

What is also important to mention - the IR v11 is fully compatible with old applications written with Inference Engine API from older versions of OpenVINO. This is achieved by adding additional runtime information to the IR v11 which is responsible for backward compatible behavior. So, once the IR v11 is read by the old Inference Engine based application, it's internally converted to IR v10 to provide backward-compatible behavior.

The IR v11 is supported by all OpenVINO Development tools including Post-Training Optimization tool, Benchmark app, etc.

### IR v10 Compatibility

API 2.0 also supports models in IR v10 for backward compatibility. So, if a user has an IR v10, it can be fed to OpenVINO Runtime as well (see [migration steps](common_inference_pipeline.md)).

Some OpenVINO Development Tools also support both IR v10 and IR v11 as an input:
- Accuracy checker also supports IR v10, but requires an additional option to denote which API is used underneath.
- [Compile tool](../../../tools/compile_tool/README.md) compiles the model to be used in API 2.0 by default. If a user wants to use the resulting compiled blob in Inference Engine API, the additional `ov_api_1_0` option should be passed.

The following OpenVINO tools don't support IR v10 as an input, and require to generate an IR v11 from the original model with the latest version of Model Optimizer:
- Post-Training Optimization tool
- Deep Learning Workbench

> **NOTE**: If you need to quantize your IR v10 models to run with OpenVINO 2022.1, it's recommended to download and use Post-Training Optimization tool from OpenVINO 2021.4 release.

### Differences between Inference Engine and OpenVINO Runtime 2022.1

Inference Engine and nGraph APIs are not deprecated, they are fully functional and can be used in applications. However, it's highly recommended to migrate to API 2.0, because it already has additional features and this list will be extended later. The following list of additional features is supported by API 2.0:
- [Working with dynamic shapes](../ov_dynamic_shapes.md). The feature is quite useful for best performance for NLP (Neural Language Processing) models, super resolution models and other which accepts dynamic input shapes.
- [Preprocessing of the model](../preprocessing_overview.md) to add preprocessing operations to the inference models and fully occupy the accelerator and free CPU resources.

To define a difference on the API level between Inference Engine and API 2.0, let's define two types of behaviors:
- **Old behavior** of OpenVINO supposes:
  - Model Optimizer can change input element types, order of dimensions (layouts) with compare to the model from the original framework.
  - Inference Engine can override input and output element types.
  - Inference Engine API operates with operation names to address inputs and outputs (e.g. InferenceEngine::InferRequest::GetBlob).
  - Does not support compiling of models with dynamic input shapes.
- **New behavior** assumes full model alignment with the framework and is implemented in OpenVINO 2022.1:
  - Model Optimizer preserves the input element types, order of dimensions (layouts) and stores tensor names from the original models.
  - OpenVINO Runtime 2022.1 reads models in any formats (IR v10, IR v11, ONNX, PaddlePaddle, etc) as is.
  - API 2.0 operates with tensor names. Note, the difference between tensor names and operations names is that in case if a single operation has several output tensors, such tensors cannot identified in a unique manner, so tensor names are used for addressing as it's usually done in the frameworks.
  - API 2.0 can address input and outputs tensors also by its index. Some model formats like ONNX are sensitive to order of inputs, outputs and its preserved by OpenVINO 2022.1.

The table below demonstrates which behavior **old** or **new** is used depending on a model source, used APIs.

|               API             | IR v10  | IR v11  | ONNX file | Model created in code |
|-------------------------------|---------|---------|-----------|-----------------------|
|Inference Engine / nGraph APIs |     Old |     Old |       Old |                   Old |
|API 2.0                        |     Old |     New |       New |                   New |

Please look at next transition guides to understand how migrate Inference Engine-based application to API 2.0:
 - [Installation & Deployment](deployment_migration.md)
 - [OpenVINO™ Common Inference pipeline](common_inference_pipeline.md)
 - [Preprocess your model](./preprocessing.md)
 - [Configure device](./configure_devices.md)
 - [OpenVINO™ Model Creation](graph_construction.md)

[tf_openvino]: ../../img/tf_openvino.png
