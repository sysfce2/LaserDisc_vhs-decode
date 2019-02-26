/************************************************************************

    efmdecoder.cpp

    ld-efm-sampletodata - EFM sample to data processor for ld-decode
    Copyright (C) 2019 Simon Inns

    This file is part of ld-decode-tools.

    ld-efm-sampletodata is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#include "efmdecoder.h"

EfmDecoder::EfmDecoder()
{
    // Initialise the state machine
    currentState = state_initial;
    nextState = currentState;
    waitingForDeltas = false;

    // Default the success tracking variables
    decodePass = 0;
    decodeFailed = 0;
    syncLoss = 0;
    efmTranslationFail = 0;
    poorSync = 0;
}

// Get the number of decodes that passed one the first try
qint32 EfmDecoder::getPass(void)
{
    return decodePass;
}

// Get the number of decodes that failed
qint32 EfmDecoder::getFailed(void)
{
    return decodeFailed;
}

// Get the number of sync losses
qint32 EfmDecoder::getSyncLoss(void)
{
    return syncLoss;
}

// Get the number of EFM translations that failed
qint32 EfmDecoder::getFailedEfmTranslations(void)
{
    return efmTranslationFail;
}

// Is an F3 frame ready?
qint32 EfmDecoder::f3FramesReady(void)
{
    return f3Frames.size();
}

// Get the F3 frame
QByteArray EfmDecoder::getF3Frames(void)
{
    QByteArray outputData;
    outputData.resize(f3Frames.size() * 34);

    qint32 pointer = 0;
    for (qint32 frame = 0; frame < f3Frames.size(); frame++) {
        // Copy the 34 byte frame
        for (qint32 byteC = 0; byteC < 34; byteC++) {
            outputData[pointer] = static_cast<char>(f3Frames[frame].outputF3Data[byteC]);
            pointer++;
        }
    }

    f3Frames.clear();
    return outputData;
}

// Process the state machine
void EfmDecoder::process(QVector<qint32> &pllResult)
{
    waitingForDeltas = false;

    while (!waitingForDeltas) {
        currentState = nextState;

        switch (currentState) {
        case state_initial:
            nextState = sm_state_initial();
            break;
        case state_findInitialSyncStage1:
            nextState = sm_state_findInitialSyncStage1(pllResult);
            break;
        case state_findInitialSyncStage2:
            nextState = sm_state_findInitialSyncStage2(pllResult);
            break;
        case state_findSecondSync:
            nextState = sm_state_findSecondSync(pllResult);
            break;
        case state_syncLost:
            nextState = sm_state_syncLost();
            break;
        case state_processFrame:
            nextState = sm_state_processFrame(pllResult);
            break;
        }
    }
}

EfmDecoder::StateMachine EfmDecoder::sm_state_initial(void)
{
    return state_findInitialSyncStage1;
}

// Search for the first T11+T11 sync pattern in the input buffer
EfmDecoder::StateMachine EfmDecoder::sm_state_findInitialSyncStage1(QVector<qint32> &pllResult)
{
    // Find the first T11+T11 sync pattern in the input buffer
    qint32 startSyncTransition = -1;

    for (qint32 i = 0; i < pllResult.size() - 1; i++) {
        if (pllResult[i] == 11 && pllResult[i + 1] == 11) {
            startSyncTransition = i;
            break;
        }
    }

    if (startSyncTransition == -1) {
        qDebug() << "EfmDecoder::sm_state_findInitialSyncStage1(): No initial sync found in input buffer, requesting more data";

        // Discard the transitions already tested and try again
        removePllResults(pllResult.size() - 1, pllResult);

        waitingForDeltas = true;
        return state_findInitialSyncStage1;
    }

    qDebug() << "EfmDecoder::sm_state_findInitialSyncStage1(): Initial sync found at transition" << startSyncTransition;

    // Discard all transitions up to the sync start (so the pllResult is the start of frame T11)
    removePllResults(startSyncTransition, pllResult);

    // Move to find initial sync stage 2
    return state_findInitialSyncStage2;
}

EfmDecoder::StateMachine EfmDecoder::sm_state_findInitialSyncStage2(QVector<qint32> &pllResult)
{
    // Find the next T11+T11 sync pattern in the input buffer
    endSyncTransition = -1;
    qint32 tTotal = 11;

    for (qint32 i = 1; i < pllResult.size() - 1; i++) {
        if (pllResult[i] == 11 && pllResult[i + 1] == 11) {
            endSyncTransition = i;
            break;
        }
        tTotal += pllResult[i];
    }

    if (endSyncTransition == -1) {
        qDebug() << "EfmDecoder::sm_state_findInitialSyncStage2(): No second sync found in input buffer, requesting more data.  T =" << tTotal;

        waitingForDeltas = true;
        return state_findInitialSyncStage2;
    }

    qDebug() << "EfmDecoder::sm_state_findInitialSyncStage2(): Found second initial sync at" << endSyncTransition;

    // Is the frame length valid?
    if (tTotal != 588) {
        // Discard the transitions already tested and try again
        qDebug() << "EfmDecoder::sm_state_findInitialSyncStage2(): Invalid T length of" << tTotal << " - trying again";
        removePllResults(endSyncTransition, pllResult);
        return state_findInitialSyncStage2;
    }

    qDebug() << "Found first F3 frame with a valid length of 588 bits";
    return state_processFrame;
}

EfmDecoder::StateMachine EfmDecoder::sm_state_findSecondSync(QVector<qint32> &pllResult)
{
    // Find the next T11+T11 sync pattern in the input buffer
    endSyncTransition = -1;
    qint32 tTotal = 11;
    //qDebug() << "T# 0 =" << pllResult[0];
    for (qint32 i = 1; i < pllResult.size() - 1; i++) {
        // T3 push?
        if (pllResult[i] < 3) pllResult[i] = 3;

        // T11 push?
        if (pllResult[i] > 11) pllResult[i] = 11;

        // Correct poor sync?
        if (i == 1) pllResult[i] = 11;

        //qDebug() << "T#" << i << "=" << pllResult[i];

        // Sync?
        if (pllResult[i] == 11 && pllResult[i + 1] == 11 && tTotal > 570) {
            endSyncTransition = i;
            poorSync = 0;
            break;
        }

        // Frame length exceeded without sync detection?
        if (tTotal > 588) {
            tTotal -= pllResult[i];
            qDebug() << "tTotal exceeded T =" << tTotal << " - poor sync #" << poorSync << "detected";
            poorSync++;

            // Rather than loosing sync, we attempt to recover by guessing at the correct
            // sync position

            // Low sync values?
            if (pllResult[i] >= 10 && pllResult[i + 1] >= 10) {
                qDebug() << "Poor sync - Low sync values";
                endSyncTransition = i;
                break;
            }

            // Sync off by -1?
            if (pllResult[i - 1] >= 10 && pllResult[i] >= 10) {
                qDebug() << "Poor sync - Off by -1";
                endSyncTransition = i - 1;
                break;
            }

            // Sync off by +1?
            if (i + 2 < pllResult.size()) {
                if (pllResult[i + 1] >= 10 && pllResult[i + 2] >= 10) {
                    qDebug() << "Poor sync - Off by +1";
                    endSyncTransition = i + 1;
                    break;
                }
            }

            // Sync off by +2?
            if (i + 3 < pllResult.size()) {
                if (pllResult[i + 2] >= 10 && pllResult[i + 3] >= 10) {
                    qDebug() << "Poor sync - Off by +2";
                    endSyncTransition = i + 2;
                    break;
                }
            }

            qDebug() << "Poor sync - no sync at all";
            endSyncTransition = i-1;
            break;
        }

        tTotal += pllResult[i];
    }

    // If endSyncTransition is -1 we ran out of data before finding the
    // sync.
    if (endSyncTransition == -1) {
        // Indicate that more deltas are required and stay in this state
        waitingForDeltas = true;
        return state_findSecondSync;
    }

    // Hit limit of poor sync detections?
    if (poorSync > 16) {
        poorSync = 0;
        qDebug() << "Too many poor sync detections (>16) - sync lost";
        return state_syncLost;
    }

    //qDebug() << "EfmDecoder::sm_state_findSecondSync(): End of F3 frame found at transition" << endSyncTransition << "T total =" << tTotal;

    // Move to the process frame state
    return state_processFrame;
}

EfmDecoder::StateMachine EfmDecoder::sm_state_syncLost(void)
{
    qDebug() << "EfmDecoder::sm_state_syncLost(): SYNC was completely lost! ------------------------------------------------------------";
    syncLoss++;
    return state_findInitialSyncStage1;
}

EfmDecoder::StateMachine EfmDecoder::sm_state_processFrame(QVector<qint32> &pllResult)
{
    QVector<qint32> frameT;
    qint32 tTotal = 0;
    for (qint32 delta = 0; delta < endSyncTransition; delta++) {
        qint32 value = pllResult[delta];

        tTotal += value;
        frameT.append(static_cast<qint32>(value));
    }
    if (tTotal == 588) {
        //qDebug() << "EfmDecoder::sm_state_processFrame(): F3 frame length ok";
        decodePass++;
    } else {
        qDebug() << "EfmDecoder::sm_state_processFrame(): F3 frame length incorrect T =" << tTotal;
        decodeFailed++;
    }

    // Discard all transitions up to the sync end
    removePllResults(endSyncTransition, pllResult);

    // Translate the F3 frame T results into a bit-stream of data
    f3Frames.resize(f3Frames.size() + 1);
    convertTvaluesToData(frameT, f3Frames[f3Frames.size() - 1].outputF3Data);

    // Find the next sync position
    return state_findSecondSync;
}

// Utility functions --------------------------------------------------------------------------------------------------

// Method to remove deltas from the start of the buffer
void EfmDecoder::removePllResults(qint32 number, QVector<qint32> &pllResult)
{
    if (number > pllResult.size()) {
        pllResult.clear();
    } else {
        for (qint32 count = 0; count < number; count++) pllResult.removeFirst();
    }
}

// This method takes a vector of T values and returns a byte array
// of 8-bit decoded data (33 bytes per F3 frame)
void EfmDecoder::convertTvaluesToData(QVector<qint32> frameT, uchar* outputData)
{
    // Firstly we have to make a bit-stream of the 588 channel bits including
    // all of the sync pattern and merge bits
    uchar rawFrameData[74];
    for (qint32 byteC = 0; byteC < 74; byteC++) rawFrameData[byteC] = 0;

    qint32 bitPosition = 7;
    qint32 bytePosition = 0;
    uchar byteData = 0;

    // Verify that the input values add up to 588 bits
    qint32 bitCount = 0;
    for (qint32 tNumber = 0; tNumber < frameT.size(); tNumber++) {
        bitCount += frameT[tNumber];
        //qDebug() << "frameT =" << frameT[tNumber];
    }

    if (bitCount != 588) qDebug() << "EfmDecoder::convertTvaluesToData(): Illegal F3 frame length";

    for (qint32 tPosition = 0; tPosition < frameT.size(); tPosition++) {
        for (qint32 bitCount = 0; bitCount < frameT[tPosition]; bitCount++) {
            if (bitCount == 0) byteData |= (1 << bitPosition);
            bitPosition--;

            if (bitPosition < 0) {
                rawFrameData[bytePosition] = byteData;
                byteData = 0;
                bitPosition = 7;
                bytePosition++;
            }
        }
    }

    // Add in the last nybble to get from 73.5 to 74 bytes
    rawFrameData[bytePosition] = byteData;

    //qDebug() << "F3Frame data:" << dataToString(rawFrameData, 74);

    // Secondly, we take the bit stream and extract just the EFM values it contains
    // There are 33 EFM values per F3 frame

    // Composition of an EFM packet is as follows:
    //
    //  1 * (24 + 3) bits sync pattern         =  27
    //  1 * (14 + 3) bits control and display  =  17
    // 32 * (14 + 3) data+parity               = 544
    //                                   total = 588 bits

    // Which demodulates to and F3 frame of:
    //
    // Sync Pattern (discarded)
    //  1 byte control
    // 32 bytes data+parity
    //
    // Total of 33 bytes

    QVector<quint32> efmValues;
    efmValues.resize(33);
    qint32 currentBit = 0;

    // Ignore the sync pattern (which is 24 bits plus 3 merging bits)
    // To-do: check the sync pattern; could be useful debug
    currentBit += 24 + 3;

    // Get the 33 x 14-bit EFM values
    for (qint32 counter = 0; counter < 33; counter++) {
        efmValues[counter] = getBits(rawFrameData, currentBit, 14);
        currentBit += 14 + 3; // the value plus 3 merging bits
        //qDebug() << "efmValues =" << efmValues[counter];
    }

    // Thirdly we take each EFM value, look it up and replace it with the
    // 8-bit value it represents

    // Note: Each output F3 frame consists of 34 bytes.  1 byte of sync data and
    // 33 bytes of actual F3 data.  We add the additional 1 byte so F3 frame
    // sync can be performed later (it's not a real F3 data byte, but otherwise
    // the SYNC0 and SYNC1 would be lost as they cannot be converted as EFM values)
    outputData[0] = 0; // No sync
    if (efmValues[0] == 0x801) outputData[0] = 0x01; // SYNC0
    if (efmValues[0] == 0x012) outputData[0] = 0x02; // SYNC1

    for (qint32 counter = 1; counter < 34; counter++) {
        qint32 result = -1;

        if (counter == 1 && (efmValues[0] == 0x801 || efmValues[0] == 0x012)) {
            // Sync bit, can't translate, so set data to 0
            outputData[counter] = 0;
        } else {
            // Normal EFM - translate to 8-bit value
            for (quint32 lutPos = 0; lutPos < 256; lutPos++) {
                if (efm2numberLUT[lutPos] == efmValues[counter - 1]) {
                    outputData[counter] = static_cast<uchar>(lutPos);
                    result = 1;
                    break;
                }
            }
        }

        if (result == -1) {
            // To-Do: count the EFM decode failures for debug
            qDebug() << "EfmDecoder::convertTvaluesToData(): 14-bit EFM value" << efmValues[counter - 1] << "not found in translation look-up table";
            efmTranslationFail++;
            outputData[counter] = 0;
        }
    }

    //qDebug() << "Output data =" << dataToString(outputData, 34);
}

// Method to get 'width' bits (max 32) from a byte array starting from
// bit 'bitIndex'
quint32 EfmDecoder::getBits(uchar *rawData, qint32 bitIndex, qint32 width)
{

    qint32 byteIndex = bitIndex / 8;
    qint32 bitInByteIndex = 7 - (bitIndex % 8);

    quint32 result = 0;
    for (qint32 nBits = width - 1; nBits > -1; nBits--) {
        if (rawData[byteIndex] & (1 << bitInByteIndex)) result += (1 << nBits);

        bitInByteIndex--;
        if (bitInByteIndex < 0) {
            bitInByteIndex = 7;
            byteIndex++;
        }
    }

    return result;
}

// This method is for debug and outputs an array of 8-bit unsigned data as a hex string
QString EfmDecoder::dataToString(uchar *data, qint32 length)
{
    QString output;

    for (qint32 count = 0; count < length; count++) {
        output += QString("%1").arg(data[count], 2, 16, QChar('0'));
    }

    return output;
}
