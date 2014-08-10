/*
	This file is part of EqualizerAPO, a system-wide equalizer.
	Copyright (C) 2014  Jonas Thedering

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License along
	with this program; if not, write to the Free Software Foundation, Inc.,
	51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#define _USE_MATH_DEFINES
#include <cmath>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <exception>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Shlwapi.h>
#include <Ks.h>
#include <KsMedia.h>
#include <mpPackageCommon.h>
#include <mpPackageNonCmplx.h>
#include <mpPackageStr.h>
#include <mpPackageMatrix.h>

#include "helpers/RegistryHelper.h"
#include "helpers/StringHelper.h"
#include "helpers/LogHelper.h"
#include "helpers/MemoryHelper.h"
#include "FilterEngine.h"
#include "filters/ExpressionFilterFactory.h"
#include "filters/DeviceFilterFactory.h"
#include "filters/StageFilterFactory.h"
#include "filters/IfFilterFactory.h"
#include "filters/ChannelFilterFactory.h"
#include "filters/BiQuadFilterFactory.h"
#include "filters/IIRFilterFactory.h"
#include "filters/PreampFilterFactory.h"
#include "filters/DelayFilterFactory.h"
#include "filters/CopyFilterFactory.h"
#include "filters/IncludeFilterFactory.h"

using namespace std;
using namespace mup;

FilterEngine::FilterEngine()
	:parser(0)
{
	lfx = false;
	capture = false;
	inputChannelCount = 0;
	lastInputWasSilent = false;
	lastInputSize = -1;
	threadHandle = NULL;
	currentConfig = NULL;
	nextConfig = NULL;
	previousConfig = NULL;
	transitionCounter = 0;
	InitializeCriticalSection(&loadSection);
	loadSemaphore = CreateSemaphore(NULL, 1, 1, NULL);
	parser.EnableAutoCreateVar(true);

	factories.push_back(new DeviceFilterFactory());
	factories.push_back(new IfFilterFactory());
	factories.push_back(new ExpressionFilterFactory());
	factories.push_back(new IncludeFilterFactory());
	factories.push_back(new StageFilterFactory());
	factories.push_back(new ChannelFilterFactory());
	factories.push_back(new IIRFilterFactory());
	factories.push_back(new BiQuadFilterFactory());
	factories.push_back(new PreampFilterFactory());
	factories.push_back(new DelayFilterFactory());
	factories.push_back(new CopyFilterFactory());

	channelNameToPosMap[L"L"] = SPEAKER_FRONT_LEFT;
	channelNameToPosMap[L"R"] = SPEAKER_FRONT_RIGHT;
	channelNameToPosMap[L"C"] = SPEAKER_FRONT_CENTER;
	channelNameToPosMap[L"SUB"] = SPEAKER_LOW_FREQUENCY;
	channelNameToPosMap[L"RL"] = SPEAKER_BACK_LEFT;
	channelNameToPosMap[L"RR"] = SPEAKER_BACK_RIGHT;
	channelNameToPosMap[L"RC"] = SPEAKER_BACK_CENTER;
	channelNameToPosMap[L"SL"] = SPEAKER_SIDE_LEFT;
	channelNameToPosMap[L"SR"] = SPEAKER_SIDE_RIGHT;

	for(hash_map<wstring, int>::iterator it=channelNameToPosMap.begin(); it!=channelNameToPosMap.end(); it++)
		channelPosToNameMap[it->second] = it->first;
}

FilterEngine::~FilterEngine()
{
	// Make sure notification thread is terminated before cleaning up, otherwise deleted memory might be accessed in loadConfig
	if(threadHandle != NULL)
	{
		SetEvent(shutdownEvent);
		if(WaitForSingleObject(threadHandle, INFINITE) == WAIT_OBJECT_0)
		{
			TraceF(L"Successfully terminated directory change notification thread");
		}
		CloseHandle(shutdownEvent);
		CloseHandle(threadHandle);
		threadHandle = NULL;
	}

	cleanupConfigurations();

	for(vector<IFilterFactory*>::iterator it = factories.begin(); it != factories.end(); it++)
		delete *it;

	CloseHandle(loadSemaphore);
	DeleteCriticalSection(&loadSection);
}

void FilterEngine::setLfx(bool lfx)
{
	this->lfx = lfx;
}

void FilterEngine::setDeviceInfo(bool capture, const wstring& deviceName, const wstring& connectionName, const wstring& deviceGuid)
{
	this->capture = capture;
	this->deviceName = deviceName;
	this->connectionName = connectionName;
	this->deviceGuid = deviceGuid;
}

void FilterEngine::initialize(float sampleRate, unsigned inputChannelCount, unsigned realChannelCount, unsigned outputChannelCount, unsigned channelMask, unsigned maxFrameCount)
{
	EnterCriticalSection(&loadSection);

	cleanupConfigurations();

	this->sampleRate = sampleRate;
	this->inputChannelCount = inputChannelCount;
	this->realChannelCount = realChannelCount;
	this->outputChannelCount = outputChannelCount;
	this->maxFrameCount = maxFrameCount;
	this->transitionCounter = 0;
	this->transitionLength = (unsigned)(sampleRate / 100);

	unsigned deviceChannelCount;
	if(capture)
		deviceChannelCount = inputChannelCount;
	else
		deviceChannelCount = outputChannelCount;

	if(channelMask == 0)
	{
		switch(deviceChannelCount)
		{
		case 1:
			channelMask = KSAUDIO_SPEAKER_MONO;
			break;
		case 2:
			channelMask = KSAUDIO_SPEAKER_STEREO;
			break;
		case 4:
			channelMask = KSAUDIO_SPEAKER_QUAD;
			break;
		case 6:
			channelMask = KSAUDIO_SPEAKER_5POINT1_SURROUND;
			break;
		case 8:
			channelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
			break;
		}
	}
	this->channelMask = channelMask;

	wstringstream channelStream;
	unsigned c=0;
	for(int i=0; i<31; i++)
	{
		int channelPos = 1<<i;
		if(channelMask & channelPos)
		{
			c++;
			if(channelStream.tellp() > 0)
				channelStream << L" ";
			if(channelPosToNameMap.find(channelPos) != channelPosToNameMap.end())
				channelStream << channelPosToNameMap[channelPos];
			else
				channelStream << c;
		}
	}
	TraceF(L"%d channels for this device: %s", deviceChannelCount, channelStream.str().c_str());

	try
	{
		configPath = RegistryHelper::readValue(APP_REGPATH, L"ConfigPath");
	}
	catch(RegistryException e)
	{
		LogF(L"Can't read config path because of: %s", e.getMessage().c_str());
		LeaveCriticalSection(&loadSection);
		return;
	}

	parser.ClearConst();
	parser.ClearFun();
	parser.ClearInfixOprt();
	parser.ClearOprt();
	parser.ClearPostfixOprt();
	parser.AddPackage(PackageCommon::Instance());
	parser.AddPackage(PackageNonCmplx::Instance());
	parser.AddPackage(PackageStr::Instance());
	parser.AddPackage(PackageMatrix::Instance());

	for(vector<IFilterFactory*>::const_iterator it = factories.cbegin(); it != factories.cend(); it++)
	{
		IFilterFactory* factory = *it;
		factory->initialize(this);
	}

	if(configPath != L"")
	{
		loadConfig();

		if(threadHandle == NULL)
		{
			shutdownEvent = CreateEventW(NULL, true, false, NULL);
			threadHandle = CreateThread(NULL, 0, notificationThread, this, 0, NULL);
			if(threadHandle == INVALID_HANDLE_VALUE)
				threadHandle = NULL;
			else
				TraceF(L"Successfully created directory change notification thread %d for %s and its subtree", GetThreadId(threadHandle), configPath.c_str());
		}
	}
	LeaveCriticalSection(&loadSection);
}

void FilterEngine::loadConfig()
{
	EnterCriticalSection(&loadSection);
	timer.start();
	if(previousConfig != NULL)
	{
		previousConfig->~FilterConfiguration();
		MemoryHelper::free(previousConfig);
		previousConfig = NULL;
	}

	allChannelNames.clear();
	unsigned c=1;
	for(int i=0; i<31; i++)
	{
		int channelPos = 1<<i;
		if(channelMask & channelPos)
		{
			if(channelPosToNameMap.find(channelPos) != channelPosToNameMap.end())
				allChannelNames.push_back(channelPosToNameMap[channelPos]);
			else
				allChannelNames.push_back(to_wstring((unsigned long long)c));
			c++;
		}
	}

	// handle channels not covered by channelMask
	for(; c<=max(realChannelCount, outputChannelCount); c++)
		allChannelNames.push_back(to_wstring((unsigned long long)c));

	currentChannelNames = allChannelNames;
	lastChannelNames.clear();
	lastNewChannelNames.clear();
	watchRegistryKeys.clear();
	parser.ClearVar();

	for(vector<IFilterFactory*>::const_iterator it = factories.cbegin(); it != factories.cend(); it++)
	{
		IFilterFactory* factory = *it;
		vector<IFilter*> newFilters = factory->startOfConfiguration();
		if(!newFilters.empty())
			addFilters(newFilters);
	}

	loadConfig(configPath + L"\\config.txt");

	for(vector<IFilterFactory*>::const_iterator it = factories.cbegin(); it != factories.cend(); it++)
	{
		IFilterFactory* factory = *it;
		vector<IFilter*> newFilters = factory->endOfConfiguration();
		if(!newFilters.empty())
			addFilters(newFilters);
	}

	void* mem = MemoryHelper::alloc(sizeof(FilterConfiguration));
	FilterConfiguration* config = new (mem) FilterConfiguration(this, filterInfos, (unsigned)allChannelNames.size());

	filterInfos.clear();

	double loadTime = timer.stop();
	TraceF(L"Finished loading configuration after %lf milliseconds", loadTime * 1000.0);

	if(currentConfig == NULL)
		currentConfig = config;
	else
		nextConfig = config;

	LeaveCriticalSection(&loadSection);
}

void FilterEngine::loadConfig(const wstring& path)
{
	TraceF(L"Loading configuration from %s", path.c_str());

	HANDLE hFile = INVALID_HANDLE_VALUE;
	while(hFile == INVALID_HANDLE_VALUE)
	{
		hFile = CreateFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if(hFile == INVALID_HANDLE_VALUE)
		{
			DWORD error = GetLastError();
			if(error != ERROR_SHARING_VIOLATION)
			{
				LogF(L"Error while reading configuration file: %s", StringHelper::getSystemErrorString(error).c_str());
				return;
			}

			// file is being written, so wait
			Sleep(1);
		}
	}

	stringstream inputStream;

	char buf[8192];
	unsigned long bytesRead = -1;
	while(ReadFile(hFile, buf, sizeof(buf), &bytesRead, NULL) && bytesRead != 0)
	{
		inputStream.write(buf, bytesRead);
	}

	CloseHandle(hFile);

	inputStream.seekg(0);

	vector<wstring> savedChannelNames = currentChannelNames;

	for(vector<IFilterFactory*>::const_iterator it = factories.cbegin(); it != factories.cend(); it++)
	{
		IFilterFactory* factory = *it;
		vector<IFilter*> newFilters = factory->startOfFile(path);
		if(!newFilters.empty())
			addFilters(newFilters);
	}

	while(!inputStream.eof())
	{
		string encodedLine;
		getline(inputStream, encodedLine);
		if(encodedLine.size() > 0 && encodedLine[encodedLine.size()-1] == '\r')
			encodedLine.resize(encodedLine.size()-1);

		wstring line = StringHelper::toWString(encodedLine, CP_UTF8);
		if(line.find(L'\uFFFD') != -1)
			line = StringHelper::toWString(encodedLine, CP_ACP);

		size_t pos = line.find(L':');
		if(pos != -1)
		{
			wstring key = line.substr(0, pos);
			wstring value = line.substr(pos + 1);

			// allow to use indentation
			key = StringHelper::trim(key);

			for(vector<IFilterFactory*>::const_iterator it = factories.cbegin(); it != factories.cend(); it++)
			{
				IFilterFactory* factory = *it;

				vector<IFilter*> newFilters;
				try
				{
					newFilters = factory->createFilter(path, key, value);
				}
				catch(exception e)
				{
					LogF(L"%S", e.what());
				}

				if(key == L"")
					break;
				if(!newFilters.empty())
				{
					addFilters(newFilters);
					break;
				}
			}
		}
	}

	for(vector<IFilterFactory*>::const_iterator it = factories.cbegin(); it != factories.cend(); it++)
	{
		IFilterFactory* factory = *it;
		vector<IFilter*> newFilters = factory->endOfFile(path);
		if(!newFilters.empty())
			addFilters(newFilters);
	}

	// restore channels selected in outer configuration file
	currentChannelNames = savedChannelNames;
}

void FilterEngine::watchRegistryKey(const std::wstring& key)
{
	watchRegistryKeys.insert(key);
}

#pragma AVRT_CODE_BEGIN
void FilterEngine::process(float *output, float *input, unsigned frameCount)
{
	bool inputSilent = true;

	if(lastInputSize != frameCount)
	{
		if(lastInputSize != -1)
			LogF(L"Input size changed from %d to %d", lastInputSize, frameCount);
		lastInputSize = frameCount;
	}

	for (unsigned i = 0; i < frameCount * realChannelCount; i++)
	{
		if(input[i] != 0)
		{
			inputSilent = false;
			break;
		}
	}

	if(inputSilent)
	{
		if(lastInputWasSilent)
		{
			//Avoid processing cost if silence would be output anyway
			if(input != output)
				memset(output, 0, frameCount * outputChannelCount * sizeof(float));

			return;
		}
		else
			lastInputWasSilent = true;
	}
	else
		lastInputWasSilent = false;

	if(currentConfig->isEmpty() && nextConfig == NULL)
	{
		// avoid (de-)interleaving cost if no processing will happen anyway
		if(realChannelCount == outputChannelCount)
		{
			if(input != output)
				memcpy(output, input, outputChannelCount * frameCount * sizeof(float));

			return;
		}
	}

	currentConfig->process(input, frameCount);

	if(nextConfig != NULL)
	{
		nextConfig->process(input, frameCount);
		float** currentSamples = currentConfig->getOutputSamples();
		float** nextSamples = nextConfig->getOutputSamples();

		for(unsigned f=0; f<frameCount; f++)
		{
			float factor = 0.5f * (1.0f - cos(transitionCounter * (float)M_PI / transitionLength));
			if(transitionCounter >= transitionLength)
				factor = 1.0f;

			for(unsigned c=0; c<outputChannelCount; c++)
				currentSamples[c][f] = currentSamples[c][f] * (1-factor) + nextSamples[c][f] * factor;

			transitionCounter++;
		}
	}

	currentConfig->write(output, frameCount);

	if(nextConfig != NULL && transitionCounter >= transitionLength)
	{
		previousConfig = currentConfig;
		currentConfig = nextConfig;
		nextConfig = NULL;
		transitionCounter = 0;
		ReleaseSemaphore(loadSemaphore, 1, NULL);
	}
}
#pragma AVRT_CODE_END

void FilterEngine::addFilters(vector<IFilter*> filters)
{
	for(vector<IFilter*>::iterator it = filters.begin(); it != filters.end(); it++)
	{
		IFilter* filter = *it;
		FilterInfo* filterInfo = (FilterInfo*)MemoryHelper::alloc(sizeof(FilterInfo));
		filterInfo->filter = filter;
		filterInfo->inPlace = filter->getInPlace();
		vector<wstring> savedChannelNames = currentChannelNames;
		bool allChannels = filter->getAllChannels();
		if(allChannels)
			currentChannelNames = allChannelNames;

		if(lastChannelNames == currentChannelNames)
		{
			filterInfo->inChannelCount = 0;
			filterInfo->inChannels = NULL;
		}
		else
		{
			filterInfo->inChannelCount = currentChannelNames.size();
			filterInfo->inChannels = (size_t*)MemoryHelper::alloc(filterInfo->inChannelCount * sizeof(size_t));

			size_t c = 0;
			for(vector<wstring>::iterator it2 = currentChannelNames.begin(); it2 != currentChannelNames.end(); it2++)
			{
				vector<wstring>::iterator pos = find(allChannelNames.begin(), allChannelNames.end(), *it2);
				filterInfo->inChannels[c++] = pos - allChannelNames.begin();
			}
		}

		lastChannelNames = currentChannelNames;

		vector<wstring> newChannelNames = filter->initialize(sampleRate, maxFrameCount, currentChannelNames);

		if(filterInfo->inPlace && lastInPlace && lastNewChannelNames == newChannelNames)
		{
			filterInfo->outChannelCount = 0;
			filterInfo->outChannels = NULL;
		}
		else
		{
			filterInfo->outChannelCount = newChannelNames.size();
			filterInfo->outChannels = (size_t*)MemoryHelper::alloc(filterInfo->outChannelCount * sizeof(size_t));

			size_t c = 0;
			for(vector<wstring>::iterator it2 = newChannelNames.begin(); it2 != newChannelNames.end(); it2++)
			{
				vector<wstring>::iterator pos = find(allChannelNames.begin(), allChannelNames.end(), *it2);
				if(pos == allChannelNames.end())
				{
					filterInfo->outChannels[c++] = allChannelNames.size();
					allChannelNames.push_back(*it2);
				}
				else
				{
					filterInfo->outChannels[c++] = pos - allChannelNames.begin();
				}
			}
		}

		lastNewChannelNames = newChannelNames;
		lastInPlace = filterInfo->inPlace;
		if(!lastInPlace)
			swap(lastChannelNames, lastNewChannelNames);

		filterInfos.push_back(filterInfo);

		if(filter->getSelectChannels())
			currentChannelNames = newChannelNames;
		else
			currentChannelNames = savedChannelNames;
	}
}

void FilterEngine::cleanupConfigurations()
{
	if(currentConfig != NULL)
	{
		currentConfig->~FilterConfiguration();
		MemoryHelper::free(currentConfig);
		currentConfig = NULL;
	}

	if(nextConfig != NULL)
	{
		nextConfig->~FilterConfiguration();
		MemoryHelper::free(nextConfig);
		nextConfig = NULL;
	}

	if(previousConfig != NULL)
	{
		previousConfig->~FilterConfiguration();
		MemoryHelper::free(previousConfig);
		previousConfig = NULL;
	}
}

unsigned long __stdcall FilterEngine::notificationThread(void* parameter)
{
	FilterEngine* engine = (FilterEngine*)parameter;

	HANDLE notificationHandle = FindFirstChangeNotificationW(engine->configPath.c_str(), true, FILE_NOTIFY_CHANGE_LAST_WRITE);
	if(notificationHandle == INVALID_HANDLE_VALUE)
		notificationHandle = NULL;

	HANDLE registryEvent = CreateEventW(NULL, true, false, NULL);

	HANDLE handles[3] = {engine->shutdownEvent, notificationHandle, registryEvent};
	while(true)
	{
		vector<HKEY> keyHandles;
		for(auto it = engine->watchRegistryKeys.begin(); it != engine->watchRegistryKeys.end(); it++)
		{
			try
			{
				HKEY keyHandle = RegistryHelper::openKey(*it, KEY_NOTIFY | KEY_WOW64_64KEY);
				keyHandles.push_back(keyHandle);
				RegNotifyChangeKeyValue(keyHandle, false, REG_NOTIFY_CHANGE_LAST_SET, registryEvent, true);
			}
			catch(RegistryException e)
			{
				LogFStatic(L"%s", e.getMessage().c_str());
			}
		}

		DWORD which = WaitForMultipleObjects(3, handles, false, INFINITE);

		for(auto it = keyHandles.begin(); it != keyHandles.end(); it++)
		{
			RegCloseKey(*it);
		}

		if(which == WAIT_OBJECT_0)
		{
			//Shutdown
			break;
		}
		else
		{
			if(which == WAIT_OBJECT_0 + 1)
			{
				FindNextChangeNotification(notificationHandle);
				//Wait for second event within 10 milliseconds to avoid loading twice
				WaitForMultipleObjects(1, &notificationHandle, false, 10);
			}

			HANDLE handles[2] = {engine->shutdownEvent, engine->loadSemaphore};
			DWORD which = WaitForMultipleObjects(2, handles, false, INFINITE);
			if(which == WAIT_OBJECT_0)
			{
				//Shutdown
				break;
			}

			engine->loadConfig();
			FindNextChangeNotification(notificationHandle);
			ResetEvent(registryEvent);
		}
	}

	FindCloseChangeNotification(notificationHandle);
	CloseHandle(registryEvent);

	return 0;
}