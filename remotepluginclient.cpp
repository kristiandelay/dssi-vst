/* -*- c-basic-offset: 4 -*- */

/*
  dssi-vst: a DSSI plugin wrapper for VST effects and instruments
  Copyright 2004-2010 Chris Cannam
*/

#include "remotepluginclient.h"

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <iostream>
#include <errno.h>
#include <stdlib.h>
#include <cstdio>
#include <string.h>

#include "rdwrops.h"

RemotePluginClient::RemotePluginClient() :
    m_controlRequestFd(-1),
    m_controlResponseFd(-1),
    m_processFd(-1),
    m_shmFd(-1),
    m_controlRequestFileName(0),
    m_controlResponseFileName(0),
    m_processFileName(0),
    m_shmFileName(0),
    m_shm(0),
    m_shmSize(0),
    m_bufferSize(-1),
    m_numInputs(-1),
    m_numOutputs(-1)
{
    char tmpFileBase[60];

    sprintf(tmpFileBase, "/tmp/rplugin_crq_XXXXXX");
    if (mkstemp(tmpFileBase) < 0) {
	cleanup();
	throw((std::string)"Failed to obtain temporary filename");
    }
    m_controlRequestFileName = strdup(tmpFileBase);

    unlink(m_controlRequestFileName);
    if (mkfifo(m_controlRequestFileName, 0666)) { //!!! what permission is correct here?
	perror(m_controlRequestFileName);
	cleanup();
	throw((std::string)"Failed to create FIFO");
    }

    sprintf(tmpFileBase, "/tmp/rplugin_crs_XXXXXX");
    if (mkstemp(tmpFileBase) < 0) {
	cleanup();
	throw((std::string)"Failed to obtain temporary filename");
    }
    m_controlResponseFileName = strdup(tmpFileBase);

    unlink(m_controlResponseFileName);
    if (mkfifo(m_controlResponseFileName, 0666)) {
	perror(m_controlResponseFileName);
	cleanup();
	throw((std::string)"Failed to create FIFO");
    }

    sprintf(tmpFileBase, "/tmp/rplugin_prc_XXXXXX");
    if (mkstemp(tmpFileBase) < 0) {
	cleanup();
	throw((std::string)"Failed to obtain temporary filename");
    }
    m_processFileName = strdup(tmpFileBase);

    unlink(m_processFileName);
    if (mkfifo(m_processFileName, 0666)) {
	perror(m_processFileName);
	cleanup();
	throw((std::string)"Failed to create FIFO");
    }

    sprintf(tmpFileBase, "/tmp/rplugin_shm_XXXXXX");
    if (mkstemp(tmpFileBase) < 0) {
	cleanup();
	throw((std::string)"Failed to obtain temporary filename");
    }
    m_shmFileName = strdup(tmpFileBase);

    m_shmFd = open(m_shmFileName, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (m_shmFd < 0) {
	cleanup();
	throw((std::string)"Failed to open or create shared memory file");
    }
}

RemotePluginClient::~RemotePluginClient()
{
    cleanup();
}

void
RemotePluginClient::syncStartup()
{
    // The first (write) fd we open in a nonblocking call, with a
    // short retry loop so we can easily give up if the other end
    // doesn't appear to be responding.  We want a nonblocking FIFO
    // for this and the process fd anyway.

    bool connected = false;
    int timeout = 40;

    for (int attempt = 0; attempt < timeout; ++attempt) {

	if ((m_controlRequestFd =
	     open(m_controlRequestFileName, O_WRONLY | O_NONBLOCK)) >= 0) {
	    connected = true;
	    break;
	} else if (errno != ENXIO) {
	    // an actual error occurred
	    break;
	}

	sleep(1);
    }

    if (!connected) {
	cleanup();
	throw((std::string)"Plugin server timed out on startup");
    }

    if ((m_controlResponseFd = open(m_controlResponseFileName, O_RDONLY)) < 0) {
	cleanup();
	throw((std::string)"Failed to open control FIFO");
    }

    connected = false;

    for (int attempt = 0; attempt < 6; ++attempt) {

	if ((m_processFd = open(m_processFileName, O_WRONLY | O_NONBLOCK)) >= 0) {
	    connected = true;
	    break;
	} else if (errno != ENXIO) {
	    // an actual error occurred
	    break;
	}

	sleep(1);
    }
	
    if (!connected) {
	cleanup();
	throw((std::string)"Failed to open process FIFO");
    }

    bool b = false;
    tryRead(m_controlResponseFd, &b, sizeof(bool));
    if (!b) {
	cleanup();
	throw((std::string)"Remote plugin did not start correctly");
    }
}

void
RemotePluginClient::cleanup()
{
    if (m_shm) {
	munmap(m_shm, m_shmSize);
	m_shm = 0;
    }
    if (m_controlRequestFd >= 0) {
	close(m_controlRequestFd);
	m_controlRequestFd = -1;
    }
    if (m_controlResponseFd >= 0) {
	close(m_controlResponseFd);
	m_controlResponseFd = -1;
    }
    if (m_processFd >= 0) {
	close(m_processFd);
	m_processFd = -1;
    }
    if (m_shmFd >= 0) {
	close(m_shmFd);
	m_shmFd = -1;
    }
    if (m_controlRequestFileName) {
	unlink(m_controlRequestFileName);
	free(m_controlRequestFileName);
	m_controlRequestFileName = 0;
    }
    if (m_controlResponseFileName) {
	unlink(m_controlResponseFileName);
	free(m_controlResponseFileName);
	m_controlResponseFileName = 0;
    }
    if (m_processFileName) {
	unlink(m_processFileName);
	free(m_processFileName);
	m_processFileName = 0;
    }
    if (m_shmFileName) {
	unlink(m_shmFileName);
	free(m_shmFileName);
	m_shmFileName = 0;
    }
}

std::string
RemotePluginClient::getFileIdentifiers()
{
    std::string id;
    id += m_controlRequestFileName + strlen(m_controlRequestFileName) - 6;
    id += m_controlResponseFileName + strlen(m_controlResponseFileName) - 6;
    id += m_processFileName + strlen(m_processFileName) - 6;
    id += m_shmFileName + strlen(m_shmFileName) - 6;
    std::cerr << "Returning file identifiers: " << id << std::endl;
    return id;
}

void
RemotePluginClient::sizeShm()
{
    if (m_numInputs < 0 || m_numOutputs < 0 || m_bufferSize < 0) return;
    size_t sz = (m_numInputs + m_numOutputs) * m_bufferSize * sizeof(float);

    ftruncate(m_shmFd, sz);

    if (m_shm) {
	m_shm = (char *)mremap(m_shm, m_shmSize, sz, MREMAP_MAYMOVE);
    } else {
	m_shm = (char *)mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, m_shmFd, 0);
    }
    if (!m_shm) {
	std::cerr << "RemotePluginClient::sizeShm: ERROR: mmap or mremap failed for " << sz
		  << " bytes from fd " << m_shmFd << "!" << std::endl;
	m_shmSize = 0;
    } else {
	memset(m_shm, 0, sz);
	m_shmSize = sz;
	std::cerr << "client sized shm to " << sz << std::endl;
    }
}

float
RemotePluginClient::getVersion()
{
//!!! client code needs to be testing this
    writeOpcode(m_controlRequestFd, RemotePluginGetVersion);
    return readFloat(m_controlResponseFd);
}

std::string
RemotePluginClient::getName()
{
    writeOpcode(m_controlRequestFd, RemotePluginGetName);
    return readString(m_controlResponseFd);
}

std::string
RemotePluginClient::getMaker()
{
    writeOpcode(m_controlRequestFd, RemotePluginGetMaker);
    return readString(m_controlResponseFd);
}

void
RemotePluginClient::setBufferSize(int s)
{
    if (s == m_bufferSize) return;
    m_bufferSize = s;
    sizeShm();
    writeOpcode(m_controlRequestFd, RemotePluginReset);
    writeInt(m_processFd, s);
}

void
RemotePluginClient::setSampleRate(int s)
{
    writeOpcode(m_processFd, RemotePluginSetSampleRate);
    writeInt(m_processFd, s);
}

void
RemotePluginClient::reset()
{
    writeOpcode(m_processFd, RemotePluginReset);
    if (m_shmSize > 0) {
	memset(m_shm, 0, m_shmSize);
    }
}

void
RemotePluginClient::terminate()
{
    writeOpcode(m_controlRequestFd, RemotePluginTerminate);
}

int
RemotePluginClient::getInputCount()
{
    writeOpcode(m_controlRequestFd, RemotePluginGetInputCount);
    m_numInputs = readInt(m_controlResponseFd);
    sizeShm();
    return m_numInputs;
}

int
RemotePluginClient::getOutputCount()
{
    writeOpcode(m_controlRequestFd, RemotePluginGetOutputCount);
    m_numOutputs = readInt(m_controlResponseFd);
    sizeShm();
    return m_numOutputs;
}

int
RemotePluginClient::getParameterCount()
{
    writeOpcode(m_controlRequestFd, RemotePluginGetParameterCount);
    return readInt(m_controlResponseFd);
}

std::string
RemotePluginClient::getParameterName(int p)
{
    writeOpcode(m_controlRequestFd, RemotePluginGetParameterName);
    writeInt(m_controlRequestFd, p);
    return readString(m_controlResponseFd);
}

void
RemotePluginClient::setParameter(int p, float v)
{
    writeOpcode(m_processFd, RemotePluginSetParameter);
    writeInt(m_processFd, p);
    writeFloat(m_processFd, v);
}

float
RemotePluginClient::getParameter(int p)
{
    writeOpcode(m_controlRequestFd, RemotePluginGetParameter);
    writeInt(m_controlRequestFd, p);
    return readFloat(m_controlResponseFd);
}

float
RemotePluginClient::getParameterDefault(int p)
{
    writeOpcode(m_controlRequestFd, RemotePluginGetParameterDefault);
    writeInt(m_controlRequestFd, p);
    return readFloat(m_controlResponseFd);
}

void
RemotePluginClient::getParameters(int p0, int pn, float *v)
{
    writeOpcode(m_controlRequestFd, RemotePluginGetParameters);
    writeInt(m_controlRequestFd, p0);
    writeInt(m_controlRequestFd, pn);
    tryRead(m_controlResponseFd, v, (pn - p0 + 1) * sizeof(float));
}

bool
RemotePluginClient::hasMIDIInput()
{
    writeOpcode(m_controlRequestFd, RemotePluginHasMIDIInput);
    bool b;
    tryRead(m_controlResponseFd, &b, sizeof(bool));
    return b;
}

int
RemotePluginClient::getProgramCount()
{
    writeOpcode(m_controlRequestFd, RemotePluginGetProgramCount);
    return readInt(m_controlResponseFd);
}

std::string
RemotePluginClient::getProgramName(int n)
{
    writeOpcode(m_controlRequestFd, RemotePluginGetProgramName);
    writeInt(m_controlRequestFd, n);
    return readString(m_controlResponseFd);
}    

void
RemotePluginClient::setCurrentProgram(int n)
{
    writeOpcode(m_processFd, RemotePluginSetCurrentProgram);
    writeInt(m_processFd, n);
}

void
RemotePluginClient::sendMIDIData(unsigned char *data, int *frameoffsets, int events)
{
    writeOpcode(m_processFd, RemotePluginSendMIDIData);
    writeInt(m_processFd, events);
    tryWrite(m_processFd, data, events * 3);

    if (!frameoffsets) {
	// This should not happen with a good client, but we'd better
	// cope as well as possible with the lazy ol' degenerates
	frameoffsets = (int *)alloca(events * sizeof(int));
	memset(frameoffsets, 0, events * sizeof(int));
    }

//    std::cerr << "RemotePluginClient::sendMIDIData(" << events << ")" << std::endl;

    tryWrite(m_processFd, frameoffsets, events * sizeof(int));
}

void
RemotePluginClient::process(float **inputs, float **outputs)
{
    struct timeval start, finish;
    gettimeofday(&start, 0);

    if (m_bufferSize < 0) {
	std::cerr << "ERROR: RemotePluginClient::setBufferSize must be called before RemotePluginClient::process" << std::endl;
	return;
    }
    if (m_numInputs < 0) {
	std::cerr << "ERROR: RemotePluginClient::getInputCount must be called before RemotePluginClient::process" << std::endl;
	return;
    }
    if (m_numOutputs < 0) {
	std::cerr << "ERROR: RemotePluginClient::getOutputCount must be called before RemotePluginClient::process" << std::endl;
	return;
    }
    if (!m_shm) {
	std::cerr << "ERROR: RemotePluginClient::process: no shared memory region available" << std::endl;
	return;
    }

    size_t blocksz = m_bufferSize * sizeof(float);

    //!!! put counter in shm to indicate number of blocks processed?
    // (so we know if we've screwed up)

    // retrieve results of previous process() instead of waiting for this
    for (int i = 0; i < m_numOutputs; ++i) {
	memcpy(outputs[i], m_shm + (i + m_numInputs) * blocksz, blocksz);
    }

    for (int i = 0; i < m_numInputs; ++i) {
	memcpy(m_shm + i * blocksz, inputs[i], blocksz);
    }

    writeOpcode(m_processFd, RemotePluginProcess);

//    std::cout << "process: wrote opcode " << RemotePluginProcess << std::endl;

    gettimeofday(&finish, 0);
//	std::cout << "process: time " << finish.tv_sec - start.tv_sec
//		  << " sec, " << finish.tv_usec - start.tv_usec << " usec"
//		  << std::endl;
    return;
}

void
RemotePluginClient::setDebugLevel(RemotePluginDebugLevel level)
{
    writeOpcode(m_controlRequestFd, RemotePluginSetDebugLevel);
    tryWrite(m_controlRequestFd, &level, sizeof(RemotePluginDebugLevel));
}

bool
RemotePluginClient::warn(std::string str)
{
    writeOpcode(m_controlRequestFd, RemotePluginWarn);
    writeString(m_controlRequestFd, str);
    bool b;
    tryRead(m_controlResponseFd, &b, sizeof(bool));
    return b;
}

void
RemotePluginClient::showGUI(std::string guiData)
{
    writeOpcode(m_controlRequestFd, RemotePluginShowGUI);
    writeString(m_controlRequestFd, guiData);
}    

void
RemotePluginClient::hideGUI()
{
    writeOpcode(m_controlRequestFd, RemotePluginHideGUI);
}

//Deryabin Andrew: vst chunks support
std::vector<char> RemotePluginClient::getVSTChunk()
{
    std::cerr << "RemotePluginClient::getChunk: getting vst chunk.." << std::endl;
    writeOpcode(m_controlRequestFd, RemotePluginGetVSTChunk);
    std::vector<char> chunk = readRaw(m_controlResponseFd);
    std::cerr << "RemotePluginClient::getChunk: got vst chunk, size=" << chunk.size() << std::endl;
    return chunk;
}

void RemotePluginClient::setVSTChunk(std::vector<char> chunk)
{
    std::cerr << "RemotePluginClient::setChunk: writing vst chunk.." << std::endl;
    std::cerr << "RemotePluginClient::setChunk: read vst chunk, size=" << chunk.size() << std::endl;
    writeOpcode(m_controlRequestFd, RemotePluginSetVSTChunk);
    writeRaw(m_controlRequestFd, chunk);
}
//Deryabin Andrew: vst chunks support: end code
