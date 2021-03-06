#include "parser.h"
#include <cstdio>    // fopen, fclose, fread, fwrite, BUFSIZ

using namespace nvinfer1;

#ifndef LOGGER
#define LOGGER

class Logger : public nvinfer1::ILogger
{
public:
    void log(Severity severity, const char* msg) override {
        if ((severity == Severity::kERROR) || (severity == Severity::kINTERNAL_ERROR)) {
            std::cout << msg << "n";
        }
    }
    nvinfer1::ILogger& getTRTLogger()
    {
        return *this;
    }
} gLogger;
#endif

size_t Parser::getSizeByDim(const nvinfer1::Dims& dims)
{
    size_t size = 1;
    for (size_t i = 0; i < dims.nbDims; ++i)
    {
        size *= dims.d[i];
    }
    return size;
}

Parser::Parser(string path, int batch_sz = 1){
	this->model_path = path;
	this->batch_size = batch_sz;
	string file_extention = this->model_path.substr(this->model_path.find_last_of(".") + 1);
	if(file_extention == "onnx"){
		nvinfer1::IBuilder*builder{nvinfer1::createInferBuilder(gLogger)};
	    const auto explicitBatch = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
	    nvinfer1::INetworkDefinition* network{builder->createNetworkV2(explicitBatch)};
	 
	    TRTUniquePtr<nvonnxparser::IParser> parser{nvonnxparser::createParser(*network, gLogger)};
	    TRTUniquePtr<nvinfer1::IBuilderConfig> config{builder->createBuilderConfig()};
	    // parse ONNX
	    if (!parser->parseFromFile(this->model_path.c_str(), static_cast< int >(nvinfer1::ILogger::Severity::kINFO)))
	    {
	        std::cerr << "ERROR: could not parse the model.\n";
	        exit(0);
	    }

		IOptimizationProfile* profile = builder->createOptimizationProfile();
	    profile->setDimensions("input_1", OptProfileSelector::kMIN, Dims4{1, 128, 128, 3});
	    profile->setDimensions("input_1", OptProfileSelector::kOPT, Dims4{1, 128, 128, 3});
	    profile->setDimensions("input_1", OptProfileSelector::kMAX, Dims4{1, 128, 128, 3});

		config->addOptimizationProfile(profile);
		// allow TensorRT to use up to 1GB of GPU memory for tactic selection.
		config->setMaxWorkspaceSize(MAX_WORKSPACE);
		// use FP16 mode if possible
		if (builder->platformHasFastFp16())
		{
		    config->setFlag(nvinfer1::BuilderFlag::kFP16);
		}
		// we have only one image in batch
		builder->setMaxBatchSize(1);
	    this->engine = builder->buildEngineWithConfig(*network, *config);
	    this->context = this->engine->createExecutionContext();
	}
	else if(file_extention == "trt"){
		std::vector<char> trtModelStream_;
		size_t size{ 0 };

		std::ifstream file(this->model_path, std::ios::binary);
		if (file.good())
		{
			file.seekg(0, file.end);
			size = file.tellg();
			file.seekg(0, file.beg);
			trtModelStream_.resize(size);
			file.read(trtModelStream_.data(), size);
			file.close();
		}
		std::cout << size <<endl;
		IRuntime* runtime = createInferRuntime(gLogger);
		assert(runtime != nullptr);
		this->engine = runtime->deserializeCudaEngine(trtModelStream_.data(), size, nullptr);
		assert(this->engine != nullptr);
		this->context = this->engine->createExecutionContext();
	}
	else
		cerr << "Cannot read " << this->model_path << endl;
}

void Parser::inference(cv::Mat image){
	//create buffer
	std::vector< nvinfer1::Dims > input_dims; // we expect only one input
	std::vector< nvinfer1::Dims > output_dims; // and one output
	std::vector< void* > buffers(this->engine->getNbBindings()); // buffers for input and output data
	for (size_t i = 0; i < this->engine->getNbBindings(); ++i)
	{
	    auto binding_size = getSizeByDim(this->engine->getBindingDimensions(i)) * batch_size * sizeof(float);
	    cudaMalloc(&buffers[i], binding_size);
	    if (this->engine->bindingIsInput(i))
	    {
	        input_dims.emplace_back(this->engine->getBindingDimensions(i));
	    }
	    else
	    {
	        output_dims.emplace_back(this->engine->getBindingDimensions(i));
	    }
	}
	if (input_dims.empty() || output_dims.empty())
	{
	    std::cerr << "Expect at least one input and one output for network \n";
	    return;
	}
	this->preprocessImage(image, (float*)buffers[0], input_dims[0]);
	this->context->enqueue(batch_size, buffers.data(), 0, nullptr);
	this->postprocessResults((float *) buffers[1], output_dims[0]);
	for (void* buf : buffers)
    {
        cudaFree(buf);
    }
}
void Parser::preprocessImage(cv::Mat frame, float* gpu_input, const nvinfer1::Dims& dims){
	if (frame.empty()){
		std::cerr << "Cannot load Input image!! \n";
        exit(0);
	}
	cv::cuda::GpuMat gpu_frame;
	gpu_frame.upload(frame);

	auto input_width = 128;
	auto input_height = 128;
	auto channels = 3;
	auto input_size = cv::Size(input_width, input_height);
	// resize
	cv::cuda::GpuMat resized;
	cv::cuda::resize(gpu_frame, resized, input_size, 0, 0, cv::INTER_NEAREST);

	//normalize
	cv::cuda::GpuMat flt_image;
	resized.convertTo(flt_image, CV_32FC3, 1.f / 255.f);
	cv::cuda::subtract(flt_image, cv::Scalar(0.485f, 0.456f, 0.406f), flt_image, cv::noArray(), -1);
	cv::cuda::divide(flt_image, cv::Scalar(0.229f, 0.224f, 0.225f), flt_image, 1, -1);

	std::vector< cv::cuda::GpuMat > chw;
    for (size_t i = 0; i < channels; ++i)
    {
        chw.emplace_back(cv::cuda::GpuMat(input_size, CV_32FC1, gpu_input + i * input_width * input_height));
    }
    cv::cuda::split(flt_image, chw);
}
void Parser::postprocessResults(float *gpu_output, const nvinfer1::Dims &dims){
	// copy results from GPU to CPU
    std::vector< float > cpu_output(getSizeByDim(dims) * this->batch_size);
    cudaMemcpy(cpu_output.data(), gpu_output, cpu_output.size() * sizeof(float), cudaMemcpyDeviceToHost);

    for (int i = 0; i < cpu_output.size(); i ++)
    	cout << cpu_output.at(i) << ' ';
    cout << endl;
}

bool Parser::export_trt(){
	size_t lastindex = this->model_path.find_last_of("."); 
	string trt_filename = this->model_path.substr(0, lastindex) + ".trt"; 

	char buf[BUFSIZ];
    size_t size;
    FILE* source = fopen(this->model_path.c_str(), "rb");
    FILE* dest = fopen(trt_filename.c_str(), "wb");

    while (size = fread(buf, 1, BUFSIZ, source)) {
        fwrite(buf, 1, size, dest);
    }

    fclose(source);
    fclose(dest);

	std::ofstream engineFile(trt_filename, std::ios::binary);
    if (!engineFile)
    {
        cerr << "Cannot open engine file: " << this->model_path << std::endl;
        return false;
    }

    TRTUniquePtr<nvinfer1::IHostMemory> serializedEngine{this->engine->serialize()};
    if (serializedEngine == nullptr)
    {
        cerr << "Engine serialization failed" << std::endl;
        return false;
    }

    
    engineFile.write(static_cast<char*>(serializedEngine->data()), serializedEngine->size());
    return !engineFile.fail();
}

Parser::~Parser(){}