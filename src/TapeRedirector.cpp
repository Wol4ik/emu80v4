﻿/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2016-2018
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

#include "Pal.h"

#include "Emulation.h"
#include "PlatformCore.h"
#include "WavReader.h"
#include "Platform.h"
#include "EmuWindow.h"
#include "CloseFileHook.h" // ElapsedTimer

#include "TapeRedirector.h"

using namespace std;

TapeRedirector::TapeRedirector()
{
    m_filter = "All Files (*.*)|*";
}


TapeRedirector::~TapeRedirector()
{
    if (m_isOpen)
        closeFile();
}


void TapeRedirector::reset()
{
    closeFile();
}


void TapeRedirector::assignFile(string fileName, string rwMode)
{
    m_permanentFileName = fileName;
    m_rwMode = rwMode;
}


void TapeRedirector::openFile()
{
    if (m_isOpen)
        closeFile();

    if (m_permanentFileName == "") {
        m_fileName = palOpenFileDialog("Open rk file", m_filter + "|Wav Files (*.wav)|*.wav;*.WAV|CSW Files (*.csw)|*.csw;*.CSW", m_rwMode == "w", m_platform->getWindow());
        g_emulation->restoreFocus();
    }
    else
        m_fileName = m_permanentFileName;

    string ext;
    if (m_fileName.size() >= 4)
        ext = m_fileName.substr(m_fileName.size() - 4, 4);
    if (ext == ".wav" || ext == ".WAV" || ext == ".csw" || ext == ".CSW") {
        m_cancelled = true;
        if (m_rwMode == "r")
            g_emulation->getWavReader()->loadFile(m_fileName);
        else if (m_rwMode == "w") {
            m_wavWriter = new WavWriter(m_platform, m_fileName, ext == ".csw" || ext == ".CSW");
        }
        return;
    }

    if (m_fileName == "") {
        m_cancelled = true;
        return;
    }

    m_file.open(m_fileName, m_rwMode);
    m_isOpen = m_file.isOpen();

    m_cancelled = !m_isOpen;
    //m_read = false;

    updateTimer();
}


void TapeRedirector::closeFile()
{
    if (m_isOpen) {
        m_file.close();
        m_isOpen = false;
    }
    m_cancelled = false;
    //m_read = false;

    if (m_wavWriter) {
        delete m_wavWriter;
        m_wavWriter = nullptr;
    }

    updateTimer();
}


void TapeRedirector::setFilePos(unsigned pos)
{
    if (m_isOpen) {
        m_file.seek(pos);
    }
}


uint8_t TapeRedirector::readByte()
{
    if (!m_isOpen && !m_cancelled)
        openFile();

    if (!m_isOpen)
        return 0;

    uint8_t buf = m_file.read8();
    if (isEof()) {
        closeFile();
        //m_read = true;
    }

    updateTimer();

    return buf;
}


uint8_t TapeRedirector::readByteSkipSeq(const uint8_t* seq, int len)
{
    uint8_t firstByte = readByte();
    if (firstByte == *seq) {
        int64_t savedPos = m_file.getPos();
        int cnt = 1;
        uint8_t inByte;
        do {
            inByte = readByte();
            ++cnt;
            ++seq;
        } while (inByte == *seq && cnt < len);
        if (cnt != len && !isEof()) {
            m_file.seek(savedPos);
        } else
          firstByte = readByte();
    }
    return firstByte;
}


uint8_t TapeRedirector::peekByte()
{

    if (!m_isOpen && !m_cancelled)
        openFile();

    if (!m_isOpen)
        return 0;

    int64_t savedPos = m_file.getPos();
    uint8_t buf = m_file.read8();
    m_file.seek(savedPos);
    return buf;
}


void TapeRedirector::writeByte(uint8_t bt)
{
    if (!m_isOpen && !m_cancelled)
        openFile();

    if (m_isOpen) {
        m_file.write8(bt);
    }

    updateTimer();
}


int TapeRedirector::getPos()
{
    if (!m_isOpen)
        return 0;

    return m_file.getPos();
}


bool TapeRedirector::isEof()
{
    if (!m_isOpen)
        return true;

    return m_file.getPos() == m_file.getSize();
}


bool TapeRedirector::isOpen()
{
    return m_isOpen;
}


bool TapeRedirector::waitForSequence(const uint8_t* seq, int len)
{
    if (!m_isOpen && !m_cancelled)
        openFile();

    if (!m_isOpen)
        return false;

    int i = 0;
    while ((i < len) && !isEof()) {
        uint8_t inByte = readByte();
        if (inByte != seq[i++])
            i = 0;
    }
    return isEof();
}



bool TapeRedirector::isCancelled()
{
    return m_cancelled/* && !m_read*/;
}


bool TapeRedirector::setProperty(const string& propertyName, const EmuValuesList& values)
{
    if (EmuObject::setProperty(propertyName, values))
        return true;

    if (propertyName == "fileName") {
        m_permanentFileName = palMakeFullFileName(values[0].asString());
        return true;
    } else if (propertyName == "mode") {
        if (values[0].asString() == "r" || values[0].asString() == "w" || values[0].asString() == "rw") {
            m_rwMode = values[0].asString();
            return true;
        }
    } else if (propertyName == "filter") {
        m_filter = values[0].asString();
        return true;
    } else if (propertyName == "timeout") {
        if (values[0].isInt()) {
            m_timeout = values[0].asInt();
            return true;
        } else
            return false;
    }

    return false;
}


string TapeRedirector::getPropertyStringValue(const string& propertyName)
{
    string res;

    res = EmuObject::getPropertyStringValue(propertyName);
    if (res != "")
        return res;

    if (propertyName == "currentFile" && m_isOpen) {
        return m_fileName;
    }

    return "";
}


void TapeRedirector::updateTimer()
{
    if (m_timer && !m_isOpen) {
        m_timer->stop();
        delete m_timer;
        m_timer = nullptr;
        return;
    }

     if (m_timeout == 0 || !m_isOpen)
        return;

    if (!m_timer)
        m_timer = new CloseFileTimer(this);

    m_timer->start(m_timeout);
}
