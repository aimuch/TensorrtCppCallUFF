#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cuda_runtime_api.h>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <cassert>
#include <vector>
#include "NvInfer.h"
#include "NvUffParser.h"

#include "NvUtils.h"

#include <cstring>
#include "opencv2/highgui/highgui_c.h"
#include "opencv2/imgproc/imgproc_c.h"
#include "opencv2/core/version.hpp"

using namespace nvuffparser;
using namespace nvinfer1;
#include "common.h"

static Logger gLogger;

#define MAX_WORKSPACE (1 << 30)

#define RETURN_AND_LOG(ret, severity, message)                                              \
    do {                                                                                    \
        std::string error_message = "sample_uff_mnist: " + std::string(message);            \
        gLogger.log(ILogger::Severity::k ## severity, error_message.c_str());               \
        return (ret);                                                                       \
    } while(0)

inline int64_t volume(const Dims& d)
{
	int64_t v = 1;
	for (int64_t i = 0; i < d.nbDims; i++)
		v *= d.d[i];
	return v;
}


inline unsigned int elementSize(DataType t)
{
	switch (t)
	{
	case DataType::kFLOAT: return 4;
	case DataType::kHALF: return 2;
	case DataType::kINT8: return 1;
	}
	assert(0);
	return 0;
}


static const int INPUT_H = 128;
static const int INPUT_W = 128;
static const int INPUT_C = 3;
static const int OUTPUT_SIZE = 5;


std::string locateFile(const std::string& input)
{
    std::vector<std::string> dirs{"data/mnist/", "data/samples/mnist/"};
    return locateFile(input, dirs);
}


// simple PGM (portable greyscale map) reader
void readPGMFile(const std::string& filename,  uint8_t buffer[INPUT_H*INPUT_W])
{
    readPGMFile(locateFile(filename), buffer, INPUT_H, INPUT_W);
}


void* safeCudaMalloc(size_t memSize)
{
    void* deviceMem;
    CHECK(cudaMalloc(&deviceMem, memSize));
    if (deviceMem == nullptr)
    {
        std::cerr << "Out of memory" << std::endl;
        exit(1);
    }
    return deviceMem;
}


std::vector<std::pair<int64_t, DataType>>
calculateBindingBufferSizes(const ICudaEngine& engine, int nbBindings, int batchSize)
{
    std::vector<std::pair<int64_t, DataType>> sizes;
    for (int i = 0; i < nbBindings; ++i)
    {
        Dims dims = engine.getBindingDimensions(i);
        DataType dtype = engine.getBindingDataType(i);

        int64_t eltCount = volume(dims) * batchSize;
        sizes.push_back(std::make_pair(eltCount, dtype));
    }

    return sizes;
}


void* createMnistCudaBuffer(int64_t eltCount, DataType dtype, float *input)
{
    /* in that specific case, eltCount == INPUT_H * INPUT_W */
    assert(eltCount == INPUT_H * INPUT_W);
    assert(elementSize(dtype) == sizeof(float));

    size_t memSize = eltCount * elementSize(dtype);

    void* deviceMem = safeCudaMalloc(memSize);
    CHECK(cudaMemcpy(deviceMem, input, memSize, cudaMemcpyHostToDevice));

    // free(input);

    return deviceMem;
}


void printOutput(int64_t eltCount, DataType dtype, void* buffer)
{
    std::cout << eltCount << " eltCount" << std::endl;
    assert(elementSize(dtype) == sizeof(float));
    std::cout << "--- OUTPUT ---" << std::endl;

    size_t memSize = eltCount * elementSize(dtype);
    float* outputs = new float[eltCount];
    CHECK(cudaMemcpy(outputs, buffer, memSize, cudaMemcpyDeviceToHost));

    int maxIdx = 0;
    for (int i = 0; i < eltCount; ++i)
        if (outputs[i] > outputs[maxIdx])
            maxIdx = i;

    for (int64_t eltIdx = 0; eltIdx < eltCount; ++eltIdx)
    {
        std::cout << eltIdx << " => " << outputs[eltIdx] << "\t : ";
        if (eltIdx == maxIdx)
            std::cout << "***";
        std::cout << "\n";
    }

    std::cout << std::endl;
    delete[] outputs;
}


ICudaEngine* loadModelAndCreateEngine(const char* uffFile, int maxBatchSize, IUffParser* parser)
{
    IBuilder* builder = createInferBuilder(gLogger);
    INetworkDefinition* network = builder->createNetwork();

#if 1
    if (!parser->parse(uffFile, *network, nvinfer1::DataType::kFLOAT))
        RETURN_AND_LOG(nullptr, ERROR, "Fail to parse");
#else
    if (!parser->parse(uffFile, *network, nvinfer1::DataType::kHALF))
        RETURN_AND_LOG(nullptr, ERROR, "Fail to parse");
    builder->setHalf2Mode(true);
#endif

    /* we create the engine */
    builder->setMaxBatchSize(maxBatchSize);
    builder->setMaxWorkspaceSize(MAX_WORKSPACE);

    ICudaEngine* engine = builder->buildCudaEngine(*network);
    if (!engine)
        RETURN_AND_LOG(nullptr, ERROR, "Unable to create engine");

    /* we can clean the network and the parser */
    network->destroy();
    builder->destroy();

    return engine;
}


void execute(ICudaEngine& engine)
{
    IExecutionContext* context = engine.createExecutionContext();
    int batchSize = 1;

    int nbBindings = engine.getNbBindings();
    assert(nbBindings == 2);

    std::vector<void*> buffers(nbBindings);
    auto buffersSizes = calculateBindingBufferSizes(engine, nbBindings, batchSize);

    int bindingIdxInput = 0;
    for (int i = 0; i < nbBindings; ++i)
    {
        if (engine.bindingIsInput(i))
            bindingIdxInput = i;
        else
        {
            auto bufferSizesOutput = buffersSizes[i];
            buffers[i] = safeCudaMalloc(bufferSizesOutput.first *elementSize(bufferSizesOutput.second));
        }
    }

    auto bufferSizesInput = buffersSizes[bindingIdxInput];

    float total = 0, ms;
    int iCorrectNum = 0;
	#define MAX_LINE 1024
	FILE *fpOpen=fopen("/media/andy/Data/DevWorkSpace/Projects/imageClassifier/data/test_abs.txt","r");
	if(fpOpen==NULL)
	{
		std::cout << ">>>> Open File Fail >>>> " << std::endl;
		return;
	}
	printf(">>>>>>> Open File OK >>>>>>> \n");
	fflush(stdout);
	char strLine[MAX_LINE];
	int numberRun =0;
	while(!feof(fpOpen))
	{
		fgets(strLine, MAX_LINE, fpOpen);
		char *token = strtok(strLine, " ");
		printf(">>>>>>> id:%d,openimg %s\n", numberRun, token);
		fflush(stdout);
		IplImage* testImg = cvLoadImage(token);

		// creat original image
		IplImage *in_img = cvCreateImage(cvSize(128, 128), IPL_DEPTH_8U, 3);
		// copy src to original image

		cvResize(testImg, in_img, CV_INTER_LINEAR);


		int _size = in_img->width * in_img->height * in_img->nChannels * sizeof(float);
		float* hostInput = (float*)malloc(_size);
		float tfOutput[5];
		int i, j, k, count=0;
		// scale pixel and change HWC->CHW
		for(k= 0; k < in_img->nChannels; ++k){
			for(j = 0; j < in_img->height; ++j){
				for(i = 0; i < in_img->width; ++i){
					hostInput[count] = ((float)in_img->imageData[k*in_img->height + j*in_img->widthStep + i] - 128.0)/128.0;
				}
			}
		}

		buffers[bindingIdxInput] = createMnistCudaBuffer(bufferSizesInput.first, bufferSizesInput.second, hostInput);

        auto t_start = std::chrono::high_resolution_clock::now();
        context->execute(batchSize, &buffers[0]);
        auto t_end = std::chrono::high_resolution_clock::now();
        ms = std::chrono::duration<float, std::milli>(t_end - t_start).count();
        total += ms;

        for (int bindingIdx = 0; bindingIdx < nbBindings; ++bindingIdx)
        {
            if (engine.bindingIsInput(bindingIdx))
                continue;

            auto bufferSizesOutput = buffersSizes[bindingIdx];
            printOutput(bufferSizesOutput.first, bufferSizesOutput.second, buffers[bindingIdx]);
        }
        CHECK(cudaFree(buffers[bindingIdxInput]));

		token = strtok(NULL, " ");
		char* label = token;
		if(token == NULL){
			break;
		}
		token = strtok(NULL, " ");


		// free(hostInput);
		cvReleaseImage(&testImg);
		cvReleaseImage(&in_img);
		numberRun++;
	}
	fclose(fpOpen);
	printf(">>>>>>> Prob = %f\n",iCorrectNum*1.0/numberRun);


    total /= numberRun;
    std::cout << "Average over " << numberRun << " runs is " << total << " ms." << std::endl;

    for (int bindingIdx = 0; bindingIdx < nbBindings; ++bindingIdx)
        if (!engine.bindingIsInput(bindingIdx))
            CHECK(cudaFree(buffers[bindingIdx]));
    context->destroy();
}


int main(int argc, char** argv)
{
    string fileName("model.uff");
    std::cout << "The UFF path is : " << fileName << std::endl;

    int maxBatchSize = 1;
    auto parser = createUffParser();

    /* Register tensorflow input */
    parser->registerInput("input", DimsCHW(3, 128, 128));
    parser->registerOutput("fc3/frozen");

    ICudaEngine* engine = loadModelAndCreateEngine(fileName.c_str(), maxBatchSize, parser);

    if (!engine)
        RETURN_AND_LOG(EXIT_FAILURE, ERROR, "Model load failed");

    /* we need to keep the memory created by the parser */
    parser->destroy();

    execute(*engine);
    engine->destroy();
    shutdownProtobufLibrary();
    return EXIT_SUCCESS;
}
