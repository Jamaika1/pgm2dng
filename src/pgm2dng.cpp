// pgm2dng.cpp : Defines the entry point for the console application.
//
#include <stdio.h>
#include <vector>
#include "ctype.h"

#include "dng_color_space.h"
#include "dng_date_time.h"
#include "dng_exceptions.h"
#include "dng_file_stream.h"
#include "dng_globals.h"
#include "dng_host.h"
#include "dng_ifd.h"
#include "dng_ext_image_writer.h"
#include "dng_info.h"
#include "dng_camera_profile.h"
#include "dng_linearization_info.h"
#include "dng_mosaic_info.h"
#include "dng_negative.h"
#include "dng_preview.h"
#include "dng_render.h"
#include "dng_simple_image.h"
#include "dng_tag_codes.h"
#include "dng_tag_types.h"
#include "dng_tag_values.h"
#include "dng_xmp.h"
#include "dng_xmp_sdk.h"

#include "dng_image_writer.h"

#include <iostream>

#include "cxxopts.hpp"

#ifndef __GNUC__
static const char *helpCompiledFor = "Compiled for Windows-7/8/10 [x64]\n";
#else
static const char *helpCompiledFor = "Compiled for Linux\n";
#endif
static const char *helpCommonInfo =
"\n" \
"This software is prepared for non-commercial use only. It is free for personal and educational (including non-profit organization) use. Distribution of this software without any permission from Fastvideo is NOT allowed. NO warranty and responsibility is provided by the authors for the consequences of using it.\n" \
"\n" ;
static const char *projectName = "PGM to DNG converter";
//extern const char *helpProject;

#ifdef _WIN32
#ifndef FOPEN
#define FOPEN(fHandle,filename,mode) fopen_s(&fHandle, filename, mode)
#endif
#ifndef FOPEN_FAIL
#define FOPEN_FAIL(result) (result != 0)
#endif
#ifndef SSCANF
#define SSCANF sscanf_s
#endif
#else
#ifndef FOPEN
#define FOPEN(fHandle,filename,mode) (fHandle = fopen(filename, mode))
#endif
#ifndef FOPEN_FAIL
#define FOPEN_FAIL(result) (result == NULL)
#endif
#ifndef SSCANF
#define SSCANF sscanf
#endif
#endif

const unsigned int PGMHeaderSize = 0x40;

template<typename T> static inline T _uSnapUp(T a, T b) 
{
	return a + (b - a % b) % b;
}
int helpPrint(void) 
{
	printf("%s", projectName);
	printf("%s", helpCompiledFor);
	printf("%s", helpCommonInfo);
	return 0;
}

int usagePrint(void) 
{
	printf("Usage:\n");
	printf("pgm2dng.exe <Optiona>\n");
	printf("Where <Options> are:\n");
	printf("--in=<path/to/input/file> - path to pgm file (mandatory)\n");
	printf("--out=<path/to/output/file> - path to dng file (mandatory)\n");
	printf("--mono - write monochrome dng\n");
	printf("--dcp=<path/to/dcp/file> - path to dcp (digital camera profile) file (mandatory for color dng)\n");
	printf("--pattern=<pattern> - CFA pattern. Alowed values: RGGB, GBRG, GRBG, BGGR (mandatory for color dng)\n");
	printf("--wp=<R,G,B> - comma separated white point values (mandatory for color dng)\n");
	printf("--white=<white> - white level (optional, dafault is maximem for a given bitdepth)\n");
	printf("--black=<black> - black level (optional, default 0)\n");
	printf("--compress - compress image with lossless jpeg\n");
	return 0;
}

int loadPGM(const char *file, 
	std::vector<unsigned char>& data, 
	unsigned int &width, 
	unsigned &wPitch, 
	unsigned int &height, 
	unsigned &bitsPerChannel) 
{
	FILE *fp = NULL;

	if (FOPEN_FAIL(FOPEN(fp, file, "rb")))
		return 0;

	unsigned int startPosition = 0;

	// check header
	char header[PGMHeaderSize] = { 0 };
	while (header[startPosition] == 0) {
		startPosition = 0;
		if (fgets(header, PGMHeaderSize, fp) == NULL)
			return 0;

		while (isspace(header[startPosition])) startPosition++;
	}

	bool fvHeader = false;
	int strOffset = 2;
	if (strncmp(&header[startPosition], "P5", 2) != 0) 
	{
		return 0;
	}


	// parse header, read maxval, width and height
	unsigned int maxval = 0;
	unsigned int i = 0;
	int readsCount = 0;

	if ((i = SSCANF(&header[startPosition + strOffset], "%u %u %u", &width, &height, &maxval)) == EOF)
		i = 0;

	while (i < 3) 
	{
		if (fgets(header, PGMHeaderSize, fp) == NULL)
			return 0;

		if (header[0] == '#')
			continue;

		if (i == 0) {
			if ((readsCount = SSCANF(header, "%u %u %u", &width, &height, &maxval)) != EOF)
				i += readsCount;
		}
		else if (i == 1) {
			if ((readsCount = SSCANF(header, "%u %u", &height, &maxval)) != EOF)
				i += readsCount;
		}
		else if (i == 2) {
			if ((readsCount = SSCANF(header, "%u", &maxval)) != EOF)
				i += readsCount;
		}
	}
	bitsPerChannel = int(log(maxval + 1) / log(2));

	const int bytePerPixel = _uSnapUp<unsigned>(bitsPerChannel, 8) / 8;

	wPitch = width * bytePerPixel;

	data.resize(wPitch * height);
	unsigned char *d = data.data();

	for (i = 0; i < height; i++) {

		if (fread(&d[i * wPitch], sizeof(unsigned char), width * bytePerPixel, fp) == 0)
			return 0;

		if (bytePerPixel == 2 && !fvHeader) {
			unsigned short *p = reinterpret_cast<unsigned short *>(&d[i * wPitch]);
			for (unsigned int x = 0; x < wPitch / bytePerPixel; x++) {
				unsigned short t = p[x];
				t = (t << 8) | (t >> 8);
				p[x] = t;
			}
		}
	}

	fclose(fp);
	return 1;
}

int main(int argc, char *argv[])
{
	using namespace std;
	
	string inputFile;
	string outputFile;
	string dcpFile;
	vector<float> wp(3);
	bool compression = false;
	bool isMonochrome = false;
	cxxopts::Options options("PGM2DNG", "PGM to DNG command line converter");

	options
		.allow_unrecognised_options()
		.add_options()
		("in", "Input pgm file", cxxopts::value<std::string>())
		("out", "Output dng file", cxxopts::value<std::string>())
		("mono", "Write monochrome dng", cxxopts::value<bool>(isMonochrome), "0")
		("dcp", "dcp profile", cxxopts::value<std::string>())
		("pattern", "CFA pattern", cxxopts::value<std::string>(), "RGGB")
		("wp", "White point", cxxopts::value<std::vector<float>>())
		("white", "White level", cxxopts::value<int>())
		("black", "Black level", cxxopts::value<int>(), "0")
		("compress", "Compress image to losseless jpeg", cxxopts::value<bool>(compression), "0")
		("h,help", "help");

	
	options.parse_positional( { "in", "out", "dcp", "pattern", "wp" } );
	auto result = options.parse(argc, argv);
	
	if (result.count("help") > 0)
	{
		usagePrint();
		exit(0);
	}

	int blackLevel = -1;
	int whiteLevel = -1;

	if (result.count("in") > 0)
	{
		inputFile = result["in"].as<std::string>();
	}
	else
	{
		printf("No input file\n");
		usagePrint();
		exit(0);
	}

	if (result.count("out") > 0)
	{
		outputFile = result["out"].as<std::string>();
	}
	else
	{
		printf("No output file\n");
		usagePrint();
		exit(0);
	}

	uint16 bayerType = 1; //RGGB
	if (!isMonochrome)
	{
		if (result.count("dcp") > 0)
		{
			dcpFile = result["dcp"].as<string>();
		}
		else
		{
			printf("No DCP file\n");
			usagePrint();
			exit(0);
		}

		//RGGB 1
		//BGGR 2
		//GBRG 3
		//GRBG 0
		if (result.count("pattern") > 0)
		{
			string str = result["pattern"].as<string>();
			if (str == "RGGB")
				bayerType = 1;
			else if (str == "BGGR")
				bayerType = 2;
			else if (str == "GBRG")
				bayerType = 3;
			else if (str == "GRBG")
				bayerType = 0;
			else
			{
				printf("Invalid CFA pattern\n");
				usagePrint();
				exit(0);
			}
		}
		else
		{
			printf("No CFA pattern\n");
			usagePrint();
			exit(0);
		}

		if (result.count("wp") > 0)
		{
			const auto v = result["wp"].as<std::vector<float>>();
			if (v.size() != 3)
			{
				printf("White point should contain 3 values\n");
				usagePrint();
				exit(0);
			}

			float max = *max_element(v.begin(), v.end());
			for (int i = 0; i < 3; i++)
			{
				wp[i] = v.at(i) / max;
			}
		}
		else
		{
			printf("No white point\n");
			usagePrint();
			exit(0);
		}
	}

	if (result.count("black") > 0)
	{
		blackLevel = result["black"].as<int>();
	}
	else
		blackLevel = 0;

	if (result.count("white") > 0)
	{
		whiteLevel = result["white"].as<int>();
	}

	unsigned int width = 0;
	unsigned wPitch = 0;
	unsigned int height = 0;
	unsigned bitsPerChannel = 0;
	vector<unsigned char> pgmData;
	
	if(loadPGM(inputFile.c_str(), pgmData, width, wPitch, height, bitsPerChannel)  != 1)
	{
		printf("File %s could not be loaded\n", inputFile.c_str());
		return false;
	}

	// SETTINGS: BAYER PATTERN
	uint8 colorPlanes = 1;
	uint8 colorChannels = isMonochrome ? 1 : 3;



	// SETTINGS: Whitebalance, Orientation "normal"
	dng_orientation orient = dng_orientation::Normal();

	// SETTINGS: Names
	const char* makeerStr = "Fastvideo";
	const char* cameraModelStr = "PGM to DNG";
	const char* profileName = cameraModelStr;
	const char* profileCopyrightStr = makeerStr;

	// Calculate bit limit
	uint16 bitLimit = 0x01 << bitsPerChannel;

	unsigned short* imageData = (unsigned short*)pgmData.data() ;

	// Create DNG
	dng_host DNGHost;

	DNGHost.SetSaveDNGVersion(dngVersion_SaveDefault);
	DNGHost.SetSaveLinearDNG(false);

	dng_rect imageBounds(height, width);
	AutoPtr<dng_image> image(DNGHost.Make_dng_image(imageBounds, colorPlanes, bitsPerChannel == 8 ? ttByte : ttShort));
	dng_pixel_buffer buffer;

	buffer.fArea = imageBounds;
	buffer.fPlane = 0;
	buffer.fPlanes = 1;
	buffer.fRowStep = buffer.fPlanes * width;
	buffer.fColStep = buffer.fPlanes;
	buffer.fPlaneStep = 1;
	buffer.fPixelType = bitsPerChannel == 8 ? ttByte : ttShort;
	buffer.fPixelSize = bitsPerChannel == 8 ? TagTypeSize(ttByte) : TagTypeSize(ttShort);
	buffer.fData = pgmData.data();

	image->Put(buffer);

	AutoPtr<dng_negative> negative(DNGHost.Make_dng_negative());

	negative->SetModelName(cameraModelStr);
	negative->SetLocalName(cameraModelStr);
	if(!isMonochrome)
	{
		negative->SetColorKeys(colorKeyRed, colorKeyGreen, colorKeyBlue);
		negative->SetBayerMosaic(bayerType);

		dng_vector cameraNeutral(3);
		cameraNeutral[0] = wp.at(0);
		cameraNeutral[1] = wp.at(1);
		cameraNeutral[2] = wp.at(2);
		negative->SetCameraNeutral(cameraNeutral);

		// Add camera profile to negative
		AutoPtr<dng_camera_profile> profile(new dng_camera_profile);
		dng_file_stream profileStream(dcpFile.c_str());
		if (profile->ParseExtended(profileStream))
			negative->AddProfile(profile);
	}
	negative->SetColorChannels(colorChannels);

	// Set linearization table
	//if (compression)
	//{
	//	std::vector<unsigned short> logLut;
	//	std::vector<unsigned short> expLut;

	//	int nEntries = 1 << bitsPerChannel;

	//	logLut.resize(nEntries);
	//	expLut.resize(nEntries);
	//	//y = -41.45 * (1 - exp(0.00112408 * x))
	//	//x = log(1. + y / 41.45) / 0.00112408

	//	//y = -0.1100386 - (0.04655303 / 0.001124336) * (1 - exp(0.001124336 * x))
	//	//y = -0.1100386 - 41,4049092 * (1 - exp(0.001124336 * x))

	//	for (int j = 0; j < nEntries; j++)
	//	{
	//		logLut[j] = (unsigned short)(log(1. + j / 41.45) / 0.00112408);
	//		expLut[j] = (unsigned short)(-41.45 * (1 - exp(0.00112408 * j)));
	//	}
	//	AutoPtr<dng_memory_block> linearizetionCurve(DNGHost.Allocate(nEntries * sizeof(unsigned short)));
	//	for (int32 i = 0; i < nEntries; i++)
	//	{
	//		uint16 *pulItem = linearizetionCurve->Buffer_uint16() + i;
	//		*pulItem = (uint16)(logLut[i]);
	//	}
	//	negative->SetLinearization(linearizetionCurve);
	//}

	negative->SetBlackLevel(blackLevel);
	negative->SetWhiteLevel(whiteLevel <= 0 ? (1 << bitsPerChannel) - 1 : whiteLevel);

	negative->SetDefaultScale(dng_urational(1, 1), dng_urational(1, 1));
	negative->SetBestQualityScale(dng_urational(1, 1));
	negative->SetDefaultCropOrigin(0, 0);
	negative->SetDefaultCropSize(width, height);
	negative->SetBaseOrientation(orient);

	negative->SetBaselineExposure(0);
	negative->SetNoiseReductionApplied(dng_urational(0, 1));
	negative->SetBaselineSharpness(1);

	// DNG EXIF
	dng_exif *poExif = negative->GetExif();
	poExif->fMake.Set_ASCII(makeerStr);
	poExif->fModel.Set_ASCII(cameraModelStr);
	poExif->fMeteringMode = 2;
	poExif->fExposureBiasValue = dng_srational(0, 0);


	// Write DNG file
	negative->SetStage1Image(image);
	negative->SynchronizeMetadata();
	negative->RebuildIPTC(true);

	dng_file_stream DNGStream(outputFile.c_str(), true);
	AutoPtr<dng_ext_image_writer> imgWriter(new dng_ext_image_writer());
	imgWriter->rawBpp = bitsPerChannel;
	imgWriter->WriteDNGEx(DNGHost, DNGStream, *negative.Get(), negative->Metadata(), 0, dngVersion_SaveDefault, !compression);

	return 0;
}

