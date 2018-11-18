﻿/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2017-2018
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <string.h>

#include "Pal.h"

#include "RkSdController.h"


using namespace std;


const static unsigned c_inBufferSize = 500;
const static unsigned c_romBufferSize = 128;


RkSdController::RkSdController(std::string sdDir)
{
    m_sdDir = palMakeFullFileName(sdDir);
    if (m_sdDir[m_sdDir.size() - 1] != '/' && m_sdDir[m_sdDir.size() - 1] != '\\')
        m_sdDir += "/";
    m_romBuffer = new uint8_t[c_romBufferSize];
    memset(m_romBuffer, 0xFF, 128);

    palReadFromFile(m_sdDir + "BOOT/BOOT.RK", 0, c_romBufferSize, m_romBuffer);

    m_inBuffer = new uint8_t[c_inBufferSize];
}


RkSdController::~RkSdController()
{
    delete[] m_romBuffer;
    delete[] m_inBuffer;
    if (m_outBuffer)
        delete[] m_outBuffer;

    for (auto it = m_fileList.begin(); it != m_fileList.end(); it++)
        delete (*it);
    m_fileList.clear();

    if (m_openFileBuffer)
        delete[] m_openFileBuffer;
}


uint8_t RkSdController::getPortA()
{
    return m_outValue;
}


void RkSdController::setPortA(uint8_t value)
{
    m_inValue = value;
}


void RkSdController::setPortB(uint8_t value)
{
    m_outValue = m_romBuffer[value & 0x7F];

    switch (m_stage) {
    case CS_WAIT40:
        if (m_prevValue == 0x44 && value == 0x40)
            m_stage = CS_WAIT0;
        break;
    case CS_WAIT0:
        if (value == 0) {
            m_stage = CS_START;
        } else
            resetState();
        break;
    case CS_START:
        if (m_prevValue & 0x20 && !(value & 0x20)) {
            m_stage = CS_OKDISK;
            m_outValue = ERR_START;
        } else if (value & 0x1F)
            resetState();
        break;
    case CS_OKDISK:
        if (m_prevValue & 0x20 && !(value & 0x20)) {
            m_outValue = ERR_OK_DISK;
            m_stage = CS_PREPARE;
            m_inBufferPos = 0;
        } else if (value & 0x1F)
            resetState();
        break;
    case CS_PREPARE:
        if (m_prevValue & 0x20 && !(value & 0x20)) {
            m_stage = CS_REQUEST;
        } else if (value & 0x1F)
            resetState();
        break;
    case CS_REQUEST:
        if (m_prevValue & 0x20 && !(value & 0x20)) {
            if (m_inBufferPos < c_inBufferSize)
                m_inBuffer[m_inBufferPos++] = m_inValue;
            else {
                m_stage = CS_ANSWER;
                createErrorAnswer(ERR_RECV_STRING);
            }
            if (cmd()) {
                m_stage = CS_ANSWER;
                m_outBufferPos = 0;
            }
        } else if (value & 0x1F)
            resetState();
        break;
    case CS_ANSWER:
        if (m_prevValue & 0x20 && !(value & 0x20) && m_outBufferPos < m_outBufferSize) {
            m_outValue = m_outBuffer[m_outBufferPos++];
        } else if (value & 0x1F)
            resetState();
        break;
    }

    m_prevValue = value;
}


void RkSdController::resetState()
{
    m_stage = CS_WAIT40;
    m_prevValue = 0;
}


void RkSdController::createErrorAnswer(ErrorCode error)
{
    if (m_outBuffer)
        delete[] m_outBuffer;
    m_outBuffer = new uint8_t[1];
    m_outBufferPos = 0;
    m_outBuffer[m_outBufferPos++] = error;
    m_outBufferSize = m_outBufferPos;
}


void RkSdController::error()
{
    m_outValue = ERR_DISK_ERR;
    resetState();
}


bool RkSdController::loadRkFile(const std::string& fileName)
{
    int fileSize;

    m_execFileBuffer = palReadFile(fileName, fileSize, true);
    if (!m_execFileBuffer)
        return false;

    if (fileSize < 8) {
        delete[] m_execFileBuffer;
        return false;
    }

    m_progPtr = m_execFileBuffer;

    if ((*m_progPtr) == 0xE6) {
        m_progPtr++;
        fileSize--;
    }

    m_progBegAddr = (m_progPtr[0] << 8) | m_progPtr[1];
    uint16_t progEndAddr = (m_progPtr[2] << 8) | m_progPtr[3];
    m_progPtr += 4;
    fileSize -= 4;

    m_progLen = progEndAddr - m_progBegAddr + 1;

    if (m_progBegAddr == 0xE6E6 || m_progBegAddr == 0xD3D3 || fileSize < m_progLen + 2) {
        // Basic or EDM File
        delete[] m_execFileBuffer;
        return false;
    }
    return true;
}


bool RkSdController::cmd()
{
        switch (m_inBuffer[0]) {
        case CMD_BOOT:
            return cmdBoot();
        case CMD_VER:
            return cmdVer();
        case CMD_EXEC:
            return cmdExec();
        case CMD_FIND:
            return cmdFind();
        case CMD_OPEN:
            return cmdOpen();
        case CMD_LSEEK:
            return cmdLseek();
        case CMD_READ:
            return cmdRead();
        case CMD_WRITE:
            return cmdWrite();
        case CMD_MOVE:
            return cmdMove();
        default:
            m_outValue = ERR_INVALID_COMMAND;
            resetState();
        }
    return false;
}


bool RkSdController::cmdBoot()
{
    int pos = 0;

    if (loadRkFile(m_sdDir + "BOOT/SDBIOS.RK")) {
        if (m_outBuffer)
            delete[] m_outBuffer;
        m_outBuffer = new uint8_t[m_progLen + 7];
        m_outBuffer[pos++] = ERR_OK_ADDR;
        m_outBuffer[pos++] = m_progBegAddr & 0xFF;
        m_outBuffer[pos++] = (m_progBegAddr & 0xFF00) >> 8;
        m_outBuffer[pos++] = ERR_OK_BLOCK;
        m_outBuffer[pos++] = m_progLen & 0xFF;
        m_outBuffer[pos++] = (m_progLen & 0xFF00) >> 8;
        memcpy(m_outBuffer + pos, m_progPtr, m_progLen);
        pos += m_progLen;
        m_outBuffer[pos++] = ERR_OK_READ;
        delete[] m_execFileBuffer;
    } else
        m_outBuffer[pos++] = ERR_NO_PATH;

    m_outBufferSize = pos;
    m_outBufferPos = 0;

    return true;
}


bool RkSdController::cmdVer()
{
    int pos = 0;
    if (m_outBuffer)
        delete[] m_outBuffer;
    m_outBuffer = new uint8_t[17];
    m_outBuffer[pos++] = 1;
    memcpy(m_outBuffer + pos, m_version, 16);
    pos += 16;
    m_outBufferSize = pos;
    m_outBufferPos = 0;
    return true;
}


bool RkSdController::cmdExec()
{
    if (m_inBufferPos < 3 || m_inBuffer[m_inBufferPos - 1] != 0)
        return false;

    int pos = 0;

    if (loadRkFile(m_sdDir + (char*)(m_inBuffer + 1))) {
        if (m_outBuffer)
            delete[] m_outBuffer;
        m_outBuffer = new uint8_t[m_progLen + 7];
        m_outBuffer[pos++] = ERR_OK_ADDR;
        m_outBuffer[pos++] = m_progBegAddr & 0xFF;
        m_outBuffer[pos++] = (m_progBegAddr & 0xFF00) >> 8;
        m_outBuffer[pos++] = ERR_OK_BLOCK;
        m_outBuffer[pos++] = m_progLen & 0xFF;
        m_outBuffer[pos++] = (m_progLen & 0xFF00) >> 8;
        memcpy(m_outBuffer + pos, m_progPtr, m_progLen);
        pos += m_progLen;
        m_outBuffer[pos++] = ERR_OK_READ;
        delete[] m_execFileBuffer;
    } else
        m_outBuffer[pos++] = ERR_NO_PATH;

    m_outBufferSize = pos;
    m_outBufferPos = 0;

    return true;
}


bool RkSdController::cmdFind()
{
    if (m_inBufferPos < 4 || m_inBuffer[m_inBufferPos - 3] != 0)
        return false;

    unsigned maxItems = m_inBuffer[m_inBufferPos - 2] | (m_inBuffer[m_inBufferPos - 1] << 8);

    string dir = string((char*)(m_inBuffer + 1));

    if (dir != ":") {
        for (auto it = m_fileList.begin(); it != m_fileList.end(); it++)
            delete (*it);
        m_fileList.clear();
        dir = m_sdDir + dir;
        palGetDirContent(dir, m_fileList);
    }

    if (m_outBuffer)
        delete[] m_outBuffer;
    m_outBuffer = new uint8_t[m_fileList.size() * 21 + 1];

    unsigned pos = 0;
    unsigned n = 0;

    for (auto it = m_fileList.begin(); it != m_fileList.end();) {
        string fileName = (*it)->fileName;
        bool hasNonLatin = false;
        int nPeriods = 0;
        for (const char* ptr = fileName.c_str(); *ptr != 0; ptr++)
            if (*ptr & 0x80) {
                hasNonLatin = true;
                break;
            } else if (*ptr == '.')
                nPeriods++;
        if (hasNonLatin || nPeriods > 2) {
            delete (*it);
            it = m_fileList.erase(it);
            continue;
        }
        string::size_type periodPos = fileName.find('.');
        string baseName = fileName.substr(0, periodPos);
        if (baseName.size() > 8) {
            delete (*it);
            it = m_fileList.erase(it);
            continue;
        }
        std::transform(baseName.begin(), baseName.end(), baseName.begin(), ::toupper);

        string ext = "";
        if (periodPos != string::npos) {
            ext  = fileName.substr(periodPos + 1, string::npos);
            if (ext.size() > 3)
                continue;
            std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
        }

        m_outBuffer[pos++] = ERR_OK_ENTRY;

        memset(m_outBuffer + pos, 0x20, 11);
        memcpy(m_outBuffer + pos, baseName.c_str(), baseName.size());
        memcpy(m_outBuffer + pos + 8, ext.c_str(), ext.size());
        pos += 11;

        m_outBuffer[pos++] = (*it)->isDir ? 0x10 : 0;

        unsigned size = (*it)->size;
        m_outBuffer[pos++] = size & 0xFF;
        m_outBuffer[pos++] = (uint8_t)(size >> 8);
        m_outBuffer[pos++] = (uint8_t)(size >> 16);
        m_outBuffer[pos++] = (uint8_t)(size >> 24);

        uint16_t time = ((*it)->hour << 11) | ((*it)->minute << 5) | ((*it)->second >> 2);
        m_outBuffer[pos++] = (uint8_t)time;
        m_outBuffer[pos++] = (uint8_t)(time >> 8);

        uint16_t date = (((*it)->year - 1980) << 9) | ((*it)->month << 5) | (*it)->day;
        m_outBuffer[pos++] = (uint8_t)date;
        m_outBuffer[pos++] = (uint8_t)(date >> 8);

        delete (*it);
        it = m_fileList.erase(it);

        if (++n == maxItems)
            break;
    }

    m_outBuffer[pos++] = ERR_OK_CMD;
    m_outBufferSize = pos;
    return true;
}


bool RkSdController::cmdOpen()
{
    if (m_inBufferPos < 4 || m_inBuffer[m_inBufferPos - 1] != 0)
        return false;

    uint8_t mode = m_inBuffer[1];

    switch (mode) {
    case O_OPEN:
        m_openFileBuffer = palReadFile(m_sdDir + (char*)(m_inBuffer + 2), (int&)m_fileSize, true);
        createErrorAnswer(m_openFileBuffer ? ERR_OK_CMD : ERR_NO_PATH);
        m_filePos = 0;
        break;
    case O_CREATE:
    case O_MKDIR:
    case O_DELETE:
    default:
        createErrorAnswer(ERR_INVALID_COMMAND);
    }

    return true;
}


bool RkSdController::cmdLseek()
{
    if (m_inBufferPos < 6)
        return false;

    uint8_t mode = m_inBuffer[1];
    int32_t offset = m_inBuffer[2] + (m_inBuffer[3] << 8) + (m_inBuffer[4] << 16) + (m_inBuffer[5] << 24);

    switch (mode) {
    case 100:
    case 101:
    case 102:
        createErrorAnswer(ERR_INVALID_COMMAND);
    default:
        if (mode == 0)
            m_filePos = offset;
        else if (mode == 1)
            m_filePos += offset;
        else
            m_filePos = m_fileSize + offset;
    }

    return true;
}


bool RkSdController::cmdRead()
{
    if (m_inBufferPos < 3)
        return false;

    uint16_t bytesToRead = m_inBuffer[1] + (m_inBuffer[2] << 8);
    if (bytesToRead > m_fileSize - m_filePos)
        bytesToRead = m_fileSize - m_filePos;

    if (m_outBuffer)
        delete[] m_outBuffer;
    m_outBuffer = new uint8_t[bytesToRead + 4];

    unsigned pos = 0;

    m_outBuffer[pos++] = ERR_OK_BLOCK;
    m_outBuffer[pos++] = bytesToRead & 0xFF;
    m_outBuffer[pos++] = (uint8_t)(bytesToRead >> 8);

    memcpy(m_outBuffer + pos, m_openFileBuffer + m_filePos, bytesToRead);
    pos += bytesToRead;
    m_filePos += bytesToRead;

    m_outBuffer[pos++] = ERR_OK_READ;

    m_outBufferSize = pos;

    return true;
}


bool RkSdController::cmdWrite()
{
    error();
    return false;
}


bool RkSdController::cmdMove()
{
    error();
    return false;
}
