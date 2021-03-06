/************************************************
Copyright (c) 2016, Xilinx, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.// Copyright (c) 2015 Xilinx, Inc.
************************************************/

#include "rx_engine.hpp"

using namespace hls;

/** @ingroup rx_engine
 * Extracts tcpLength from IP header, removes the header and prepends the IP addresses to the payload,
 * such that the output can be used for the TCP pseudo header creation
 * The TCP length is computed from the total length and the IP header length
 * @param[in]		dataIn, incoming data stream
 * @param[out]		dataOut, outgoing data stream
 * @param[out]		tcpLenFifoOut, the TCP length is stored into this FIFO
 * @TODO maybe compute TCP length in another way!!
 */
void rxTcpLengthExtract(stream<axiWord>&			dataIn,
						stream<axiWord>&			dataOut,
						stream<ap_uint<16> >&		tcpLenFifoOut)
{
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	static ap_uint<8> tle_ipHeaderLen = 0;
	static ap_uint<16> tle_ipTotalLen = 0;
	static ap_uint<4> tle_wordCount = 0;
	static bool tle_insertWord = false;
	static bool tle_wasLast = false;
	static bool tle_shift = true;
	static axiWord tle_prevWord;

	axiWord currWord;
	axiWord sendWord;

	if (tle_insertWord)
	{
		sendWord.data = 0;
		sendWord.keep = 0xFF;
		sendWord.last = 0;
		dataOut.write(sendWord);
		//printWord(sendWord);
		tle_insertWord = false;
	}
	else if (!dataIn.empty() && !tle_wasLast)
	{
		dataIn.read(currWord);
		switch (tle_wordCount)
		{
		case 0:
			tle_ipHeaderLen = currWord.data(3, 0);
			tle_ipTotalLen(7, 0) = currWord.data(31, 24);
			tle_ipTotalLen(15, 8) = currWord.data(23, 16);
			tle_ipTotalLen -= (tle_ipHeaderLen * 4);
			tle_ipHeaderLen -= 2; //?
			tle_wordCount++;
			break;
		case 1:
			// Get source IP address
			// -> is put into prevWord
			// Write length
			tcpLenFifoOut.write(tle_ipTotalLen);
			tle_ipHeaderLen -= 2;
			tle_wordCount++;
			break;
		case 2:
			// Get destination IP address
			sendWord.data(31, 0) = tle_prevWord.data(63, 32);
			sendWord.keep(3, 0) = tle_prevWord.keep(7, 4);
			sendWord.data(63, 32) = currWord.data(31, 0);
			sendWord.keep(7, 4) = currWord.keep(3, 0);
			//sendWord.last = currWord.last;
			sendWord.last = (currWord.keep[4] == 0);
			dataOut.write(sendWord);
			//printWord(sendWord);
			tle_ipHeaderLen -= 1;
			tle_insertWord = true;
			tle_wordCount++;
			break;
		case 3:
			switch (tle_ipHeaderLen)
			{
			case 0: //half of prevWord contains valuable data and currWord is full of valuable
				sendWord.data(31, 0) = tle_prevWord.data(63, 32);
				sendWord.keep(3, 0) = tle_prevWord.keep(7, 4);
				sendWord.data(63, 32) = currWord.data(31, 0);
				sendWord.keep(7, 4) = currWord.keep(3, 0);
				//sendWord.last = currWord.last;
				sendWord.last = (currWord.keep[4] == 0);
				dataOut.write(sendWord);
				//printWord(sendWord);
				tle_shift = true;
				tle_ipHeaderLen = 0;
				tle_wordCount++;
				break;
			case 1: //prevWord contains shitty data, but currWord is valuable
				sendWord = currWord;
				dataOut.write(sendWord);
				//printWord(sendWord);
				tle_shift = false;
				tle_ipHeaderLen = 0;
				tle_wordCount++;
				break;
			default: //prevWord contains shitty data, currWord at least half shitty
				//Drop this shit
				tle_ipHeaderLen -= 2;
				break;
			}
			break;
		default:
			if (tle_shift)
			{
				sendWord.data(31, 0) = tle_prevWord.data(63, 32);
				sendWord.keep(3, 0) = tle_prevWord.keep(7, 4);
				sendWord.data(63, 32) = currWord.data(31, 0);
				sendWord.keep(7, 4) = currWord.keep(3, 0);
				sendWord.last = (currWord.keep[4] == 0);
				dataOut.write(sendWord);
			}
			else
			{
				sendWord = currWord;
				dataOut.write(sendWord);
			}
			break;
		} //switch on WORD_N
		tle_prevWord = currWord;
		if (currWord.last)
		{
			tle_wordCount = 0;
			tle_wasLast = !sendWord.last;
		}
	} // if !empty
	else if (tle_wasLast) //Assumption has to be shift
	{
		// Send remainng data
		sendWord.data(31, 0) = tle_prevWord.data(63, 32);
		sendWord.keep(3, 0) = tle_prevWord.keep(7, 4);
		sendWord.data(63, 32) = 0;
		sendWord.keep(7, 4) = 0x0;
		sendWord.last = 0x1;
		dataOut.write(sendWord);
		tle_wasLast = false;
	}
}

/** @ingroup rx_engine
 * Constructs the TCP pseudo header and prepends it to the TCP payload
 * @param[in]	dataIn, incoming Axi-Stream
 * @param[in]	tcpLenFifoIn, FIFO containing the TCP length of the current packet
 * @param[out]	dataOut, outgoing Axi-Stream
 */
void rxInsertPseudoHeader(stream<axiWord>&				dataIn,
							stream<ap_uint<16> >&		tcpLenFifoIn,
							stream<axiWord>&			dataOut)
{
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	static bool iph_wasLast = false;
	static ap_uint<2> iph_wordCount = 0;
	axiWord currWord, sendWord;
	static axiWord iph_prevWord;
	ap_uint<1> valid;
	ap_uint<16> tcpLen;


	currWord.last = 0;
	if (iph_wasLast)
	{
		sendWord.data(31,0) = iph_prevWord.data(63,32);
		sendWord.keep(3, 0) = iph_prevWord.keep(7,4);
		sendWord.keep(7, 4) = 0x0;
		sendWord.last = 0x1;
		dataOut.write(sendWord);
		iph_wasLast = false;
	}
	else if(!dataIn.empty())
	{
		switch (iph_wordCount)
		{
		case 0:
			dataIn.read(currWord);
			iph_wordCount++;
			break;
		case 1:
			dataIn.read(currWord);
			sendWord = iph_prevWord;
			dataOut.write(sendWord);
			iph_wordCount++;
			break;
		case 2:
			if (!tcpLenFifoIn.empty())
			{
				dataIn.read(currWord);
				tcpLenFifoIn.read(tcpLen);
				sendWord.data(15, 0) = 0x0600;
				sendWord.data(23, 16) = tcpLen(15, 8);
				sendWord.data(31, 24) = tcpLen(7, 0);
				sendWord.data(63, 32) = currWord.data(31, 0);
				sendWord.keep = 0xFF;
				sendWord.last = 0;
				dataOut.write(sendWord);
				iph_wordCount++;
			}
			break;
		default:
			dataIn.read(currWord);
			sendWord.data.range(31, 0) = iph_prevWord.data.range(63, 32);
			sendWord.data.range(63, 32) = currWord.data.range(31, 0);
			sendWord.keep.range(3, 0) = iph_prevWord.keep.range(7, 4);
			sendWord.keep.range(7, 4) = currWord.keep.range(3, 0);
			sendWord.last = (currWord.keep[4] == 0); //some "nice" stuff here
			dataOut.write(sendWord);
			break;
		}
		iph_prevWord = currWord;
		if (currWord.last == 1)
		{
			iph_wordCount = 0;
			iph_wasLast = !sendWord.last;
		}
	}
}

/** @ingroup rx_engine
 *  Checks the TCP checksum writes valid into @p validBuffer
 *  Additionally it extracts some metadata and the IP tuples from
 *  the TCP packet and writes it to @p metaDataFifoOut
 *  and @p tupleFifoOut
 *  It also sends the destination port number to the @ref port_table
 *  to check if the port is open.
 *  @param[in]		dataIn
 *  @param[out]		dataOut
 *  @param[out]		validFifoOut
 *  @param[out]		metaDataFifoOut
 *  @param[out]		tupleFifoOut
 *  @param[out]		portTableOut
 */
void rxCheckTCPchecksum(stream<axiWord>&					dataIn,
							stream<axiWord>&				dataOut,
							stream<bool>&					validFifoOut,
							stream<rxEngineMetaData>&		metaDataFifoOut,
							stream<fourTuple>&				tupleFifoOut,
							stream<ap_uint<16> >&			portTableOut)
{
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	static ap_uint<17> csa_tcp_sums[4] = {0, 0, 0, 0};
	static ap_uint<8> csa_dataOffset = 0xFF;
	static ap_uint<16> csa_wordCount = 0;
	static fourTuple csa_sessionTuple;
	static bool csa_shift = false;
	static bool csa_wasLast = false;
	static bool csa_checkChecksum = false;
	static ap_uint<36> halfWord; 
	axiWord currWord, sendWord;
	static rxEngineMetaData csa_meta;
	static ap_uint<16> csa_port;

	static ap_uint<3> csa_cc_state = 0;

	//currWord.last = 0; //mighnt no be necessary any more FIXME to you want to risk it ;)
	if (!dataIn.empty() && !csa_checkChecksum)
	{
		dataIn.read(currWord);
		switch (csa_wordCount)
		{
		case 0:
			csa_dataOffset = 0xFF;
			csa_shift = false;
				// We don't switch bytes, internally we store it Most Significant Byte Last
				csa_sessionTuple.srcIp = currWord.data(31, 0);
				csa_sessionTuple.dstIp = currWord.data(63, 32);
				sendWord.last = currWord.last;

			break;
		case 1:
			// Get length
			csa_meta.length(7, 0) = currWord.data(31, 24);
			csa_meta.length(15, 8) = currWord.data(23, 16);
			// We don't switch bytes, internally we store it Most Significant Byte Last
			csa_sessionTuple.srcPort = currWord.data(47, 32);
			csa_sessionTuple.dstPort = currWord.data(63, 48);
			csa_port = currWord.data(63, 48);
			sendWord.last = currWord.last;
			break;
		case 2:
			// GET SEQ and ACK number
			csa_meta.seqNumb(7, 0) = currWord.data(31, 24);
			csa_meta.seqNumb(15, 8) = currWord.data(23, 16);
			csa_meta.seqNumb(23, 16) = currWord.data(15, 8);
			csa_meta.seqNumb(31, 24) = currWord.data(7, 0);
			csa_meta.ackNumb(7, 0) = currWord.data(63, 56);
			csa_meta.ackNumb(15, 8) = currWord.data(55, 48);
			csa_meta.ackNumb(23, 16) = currWord.data(47, 40);
			csa_meta.ackNumb(31, 24) = currWord.data(39, 32);
			sendWord.last = currWord.last;
			break;
		case 3:
			csa_dataOffset = currWord.data.range(7, 4);
			csa_meta.length -= (csa_dataOffset * 4);
			//csa_dataOffset -= 5; //FIXME, do -5
			/* Control bits:
			 * [8] == FIN
			 * [9] == SYN
			 * [10] == RST
			 * [11] == PSH
			 * [12] == ACK
			 * [13] == URG
			 */
			csa_meta.ack = currWord.data[12];
			csa_meta.rst = currWord.data[10];
			csa_meta.syn = currWord.data[9];
			csa_meta.fin = currWord.data[8];
			csa_meta.winSize(7, 0) = currWord.data(31, 24);
			csa_meta.winSize(15, 8) = currWord.data(23, 16);
			// We add checksum as well and check for cs == 0
			sendWord.last = currWord.last;
			break;
		default:
			if (csa_dataOffset > 6)
			{
				csa_dataOffset -= 2;
			}
			else if (csa_dataOffset == 6)
			{
				csa_dataOffset = 5;
				csa_shift = true;
				halfWord.range(31, 0) = currWord.data.range(63, 32);
				halfWord.range(35, 32) = currWord.keep.range(7, 4);
				sendWord.last = (currWord.keep[4] == 0);
			}
			else // == 5 (or less)
			{
				if (!csa_shift)
				{
					sendWord = currWord;
					dataOut.write(sendWord);
				}
				else
				{
					sendWord.data.range(31, 0) = halfWord.range(31, 0);
					sendWord.data.range(63, 32) = currWord.data.range(31, 0);
					sendWord.keep.range(3, 0) = halfWord.range(35, 32);
					sendWord.keep.range(7, 4) = currWord.keep.range(3, 0);
					sendWord.last = (currWord.keep[4] == 0);
					/*if (currWord.last && currWord.strb.range(7, 4) != 0)
					{
						sendWord.last = 0;
					}*/
					dataOut.write(sendWord);
					halfWord.range(31, 0) = currWord.data.range(63, 32);
					halfWord.range(35, 32) = currWord.keep.range(7, 4);
				}
			}
			break;
		} // switch
		for (int i = 0; i < 4; i++)
		{
#pragma HLS UNROLL
			ap_uint<16> temp;
			if (currWord.keep.range(i*2+1, i*2) == 0x3)
			{
				temp(7, 0) = currWord.data.range(i*16+15, i*16+8);
				temp(15, 8) = currWord.data.range(i*16+7, i*16);
				csa_tcp_sums[i] += temp;
				csa_tcp_sums[i] = (csa_tcp_sums[i] + (csa_tcp_sums[i] >> 16)) & 0xFFFF;
			}
			else if (currWord.keep[i*2] == 0x1)
			{
				temp(7, 0) = 0;
				temp(15, 8) = currWord.data.range(i*16+7, i*16);
				csa_tcp_sums[i] += temp;
				csa_tcp_sums[i] = (csa_tcp_sums[i] + (csa_tcp_sums[i] >> 16)) & 0xFFFF;
			}
		}
		csa_wordCount++;
		if(currWord.last == 1)
		{
			csa_wordCount = 0;
			csa_wasLast = !sendWord.last; // moved length test down
			csa_checkChecksum = true;
		}
	}
	/*if (currWord.last == 1)
	{
		csa_wordCount = 0;
		csa_checkChecksum = true;
	}*/
	else if(csa_wasLast) //make if
	{
		if (csa_meta.length != 0)
		{
			sendWord.data.range(31, 0) = halfWord.range(31, 0);
			sendWord.data.range(63, 32) = 0;
			sendWord.keep.range(3, 0) = halfWord.range(35, 32);
			sendWord.keep.range(7, 4) = 0;
			sendWord.last = 1;
			dataOut.write(sendWord);
		}
		csa_wasLast = false;
	}
	else if (csa_checkChecksum) //make if?
	{
		switch (csa_cc_state)
		{
		case 0:
			csa_tcp_sums[0] = (csa_tcp_sums[0] + (csa_tcp_sums[0] >> 16)) & 0xFFFF;
			csa_tcp_sums[1] = (csa_tcp_sums[1] + (csa_tcp_sums[1] >> 16)) & 0xFFFF;
			csa_tcp_sums[2] = (csa_tcp_sums[2] + (csa_tcp_sums[2] >> 16)) & 0xFFFF;
			csa_tcp_sums[3] = (csa_tcp_sums[3] + (csa_tcp_sums[3] >> 16)) & 0xFFFF;
			csa_cc_state++;
			break;
		case 1:
			csa_tcp_sums[0] += csa_tcp_sums[2];
			csa_tcp_sums[1] += csa_tcp_sums[3];
			csa_tcp_sums[0] = (csa_tcp_sums[0] + (csa_tcp_sums[0] >> 16)) & 0xFFFF;
			csa_tcp_sums[1] = (csa_tcp_sums[1] + (csa_tcp_sums[1] >> 16)) & 0xFFFF;
			csa_cc_state++;
			break;
		case 2:
			csa_tcp_sums[0] += csa_tcp_sums[1];
			csa_tcp_sums[0] = (csa_tcp_sums[0] + (csa_tcp_sums[0] >> 16)) & 0xFFFF;
			csa_cc_state++;
			break;
		case 3:
			csa_tcp_sums[0] = ~csa_tcp_sums[0];
			csa_cc_state++;
			break;
		case 4:
			// If summation == 0 then checksum is correct
			if (csa_tcp_sums[0](15, 0) == 0)
			{
				// Since pkg is valid, write out metadata, 4-tuple and check port
				metaDataFifoOut.write(csa_meta);
				portTableOut.write(csa_port);
				tupleFifoOut.write(csa_sessionTuple);
				if (csa_meta.length != 0)
				{
					validFifoOut.write(true);
				}
			}
			else if(csa_meta.length != 0)
			{
				validFifoOut.write(false);
			}
			csa_checkChecksum = false;
			csa_tcp_sums[0] = 0;
			csa_tcp_sums[1] = 0;
			csa_tcp_sums[2] = 0;
			csa_tcp_sums[3] = 0;
			csa_cc_state = 0;
			break;
		}

	}
}


/** @ingroup rx_engine
 *  For each packet it reads the valid value from @param validFifoIn
 *  If the packet is valid the data stream is passed on
 *  If it is not valid it is dropped
 *  @param[in]		dataIn, incoming data stream
 *  @param[in]		validFifoIn, Valid FIFO indicating if current packet is valid
 *  @param[out]		dataOut, outgoing data stream
 */
void rxTcpInvalidDropper(stream<axiWord>&				dataIn,
							stream<bool>&				validFifoIn,
							stream<axiWord>&			dataOut)
{
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	enum rtid_StateType {GET_VALID, FWD, DROP};
	static rtid_StateType rtid_state = GET_VALID;

	axiWord currWord;
	bool valid;


	switch (rtid_state) {
	case GET_VALID: //Drop1
		if (!validFifoIn.empty())
		{
			validFifoIn.read(valid);
			if (valid)
			{
				rtid_state = FWD;
			}
			else
			{
				rtid_state = DROP;
			}
		}
		break;
	case FWD:
		if(!dataIn.empty() && !dataOut.full())
		{
			dataIn.read(currWord);
			dataOut.write(currWord);
			if (currWord.last)
			{
				rtid_state = GET_VALID;
			}
		}
		break;
	case DROP:
		if(!dataIn.empty())
		{
			dataIn.read(currWord);
			if (currWord.last)
			{
				rtid_state = GET_VALID;
			}
		}
		break;
	} // switch
}




/** @ingroup rx_engine
 * The module contains 2 state machines nested into each other. The outer state machine
 * loads the metadata and does the session lookup. The inner state machin then evaluates all
 * this data. This inner state machine mostly represents the TCP state machine and contains
 * all the logic how to update the metadata, what events are triggered and so on. It is the key
 * part of the @ref rx_engine.
 * @param[in]	metaDataFifoIn
 * @param[in]	sLookup2rxEng_rsp
 * @param[in]	stateTable2rxEng_upd_rsp
 * @param[in]	portTable2rxEng_rsp
 * @param[in]	tupleBufferIn
 * @param[in]	rxSar2rxEng_upd_rsp
 * @param[in]	txSar2rxEng_upd_rsp
 * @param[out]	rxEng2sLookup_req
 * @param[out]	rxEng2stateTable_req
 * @param[out]	rxEng2rxSar_upd_req
 * @param[out]	rxEng2txSar_upd_req
 * @param[out]	rxEng2timer_clearRetransmitTimer
 * @param[out]	rxEng2timer_setCloseTimer
 * @param[out]	openConStatusOut
 * @param[out]	rxEng2eventEng_setEvent
 * @param[out]	dropDataFifoOut
 * @param[out]	rxBufferWriteCmd
 * @param[out]	rxEng2rxApp_notification
 */
void rxMetadataHandler(	stream<rxEngineMetaData>&				metaDataFifoIn,
						stream<sessionLookupReply>&				sLookup2rxEng_rsp,
						stream<bool>&							portTable2rxEng_rsp,
						stream<fourTuple>&						tupleBufferIn,
						stream<sessionLookupQuery>&				rxEng2sLookup_req,
						stream<extendedEvent>&					rxEng2eventEng_setEvent,
						stream<bool>&							dropDataFifoOut,
						stream<rxFsmMetaData>&					fsmMetaDataFifo)
{
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	static rxEngineMetaData mh_meta;
	static sessionLookupReply mh_lup;
	enum mhStateType {META, LOOKUP};
	static mhStateType mh_state = META;
	static ap_uint<32> mh_srcIpAddress;
	static ap_uint<16> mh_dstIpPort;

	fourTuple tuple;
	bool portIsOpen;

	switch (mh_state)
	{
	case META:
		if (!metaDataFifoIn.empty() && !portTable2rxEng_rsp.empty() && !tupleBufferIn.empty())
		{
			metaDataFifoIn.read(mh_meta);
			portTable2rxEng_rsp.read(portIsOpen);
			tupleBufferIn.read(tuple);
			mh_srcIpAddress(7, 0) = tuple.srcIp(31, 24);
			mh_srcIpAddress(15, 8) = tuple.srcIp(23, 16);
			mh_srcIpAddress(23, 16) = tuple.srcIp(15, 8);
			mh_srcIpAddress(31, 24) = tuple.srcIp(7, 0);
			mh_dstIpPort(7, 0) = tuple.dstPort(15, 8);
			mh_dstIpPort(15, 8) = tuple.dstPort(7, 0);
			// CHeck if port is closed
			if (!portIsOpen)
			{
				// SEND RST+ACK
				if (!mh_meta.rst)
				{
					// send necesssary tuple through event
					fourTuple switchedTuple;
					switchedTuple.srcIp = tuple.dstIp;
					switchedTuple.dstIp = tuple.srcIp;
					switchedTuple.srcPort = tuple.dstPort;
					switchedTuple.dstPort = tuple.srcPort;
					if (mh_meta.syn || mh_meta.fin)
					{
						rxEng2eventEng_setEvent.write(extendedEvent(rstEvent(mh_meta.seqNumb+mh_meta.length+1), switchedTuple)); //always 0
					}
					else
					{
						rxEng2eventEng_setEvent.write(extendedEvent(rstEvent(mh_meta.seqNumb+mh_meta.length), switchedTuple));
					}
				}
				//else ignore => do nothing
				if (mh_meta.length != 0)
				{
					dropDataFifoOut.write(true);
				}
			}
			else
			{
				// Make session lookup, only allow creation of new entry when SYN or SYN_ACK
				rxEng2sLookup_req.write(sessionLookupQuery(tuple, (mh_meta.syn && !mh_meta.rst && !mh_meta.fin)));
				mh_state = LOOKUP;
			}
		}
		break;
	case LOOKUP: //BIG delay here, waiting for LOOKup
		if (!sLookup2rxEng_rsp.empty())
		{
			sLookup2rxEng_rsp.read(mh_lup);
			if (mh_lup.hit)
			{
				//Write out lup and meta
				fsmMetaDataFifo.write(rxFsmMetaData(mh_lup.sessionID, mh_srcIpAddress, mh_dstIpPort, mh_meta));
			}
			if (mh_meta.length != 0)
			{
				dropDataFifoOut.write(!mh_lup.hit);
			}
			/*if (!mh_lup.hit)
			{
				// Port is Open, but we have no sessionID, that matches or is free
				// For SYN we should time out, for everything else sent RST TODO
				if (mh_meta.length != 0)
				{
					dropDataFifoOut.write(true); // always write???
				}
				//mh_state = META;
			}
			else
			{
				//Write out lup and meta
				fsmMetaDataFifo.write(rxFsmMetaData(mh_lup.sessionID, mh_srcIpAddress, mh_dstIpPort, mh_meta));

				// read state
				/*rxEng2stateTable_upd_req.write(stateQuery(mh_lup.sessionID));
				// read rxSar & txSar
				if (!(mh_meta.syn && !mh_meta.rst && !mh_meta.fin)) // Do not read rx_sar for SYN(+ACK)(+ANYTHING) => (!syn || rst || fin
				{
					rxEng2rxSar_upd_req.write(rxSarRecvd(mh_lup.sessionID));
				}
				if (mh_meta.ack) // Do not read for SYN (ACK+ANYTHING)
				{
					rxEng2txSar_upd_req.write(rxTxSarQuery(mh_lup.sessionID));
				}*/
				//mh_state = META;
			//}
			mh_state = META;
		}

		break;
	}//switch
}

void rxTcpFSM(			stream<rxFsmMetaData>&					fsmMetaDataFifo,
						stream<sessionState>&					stateTable2rxEng_upd_rsp,
						stream<rxSarEntry>&						rxSar2rxEng_upd_rsp,
						stream<rxTxSarReply>&					txSar2rxEng_upd_rsp,
						stream<stateQuery>&						rxEng2stateTable_upd_req,
						stream<rxSarRecvd>&						rxEng2rxSar_upd_req,
						stream<rxTxSarQuery>&					rxEng2txSar_upd_req,
						stream<rxRetransmitTimerUpdate>&		rxEng2timer_clearRetransmitTimer,
						stream<ap_uint<16> >&					rxEng2timer_clearProbeTimer,
						stream<ap_uint<16> >&					rxEng2timer_setCloseTimer,
						stream<openStatus>&						openConStatusOut,
						stream<event>&							rxEng2eventEng_setEvent,
						stream<bool>&							dropDataFifoOut,
#if !(RX_DDR_BYPASS)
						stream<mmCmd>&							rxBufferWriteCmd,
						stream<appNotification>&				rxEng2rxApp_notification)
#else
						stream<appNotification>&				rxEng2rxApp_notification,
						ap_uint<32>						rxbuffer_data_count,
						ap_uint<32>						rxbuffer_max_data_count)
#endif
{
#pragma HLS INLINE off
#pragma HLS pipeline II=1


	enum fsmStateType {LOAD, TRANSITION};
	static fsmStateType fsm_state = LOAD;

	static rxFsmMetaData fsm_meta;
	static bool fsm_txSarRequest = false;


	ap_uint<4> control_bits = 0;
	sessionState tcpState;
	rxSarEntry rxSar;
	rxTxSarReply txSar;


	switch(fsm_state)
	{
	case LOAD:
		if (!fsmMetaDataFifo.empty())
		{
			fsmMetaDataFifo.read(fsm_meta);
			// read state
			rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID));
			// Always read rxSar, even though not required for SYN-ACK
			rxEng2rxSar_upd_req.write(rxSarRecvd(fsm_meta.sessionID));
			// read txSar
			if (fsm_meta.meta.ack) // Do not read for SYN (ACK+ANYTHING)
			{
				rxEng2txSar_upd_req.write(rxTxSarQuery(fsm_meta.sessionID));
				fsm_txSarRequest  = true;
			}
			fsm_state = TRANSITION;
		}
		break;
	case TRANSITION:
		// Check if transition to LOAD occurs
		if (!stateTable2rxEng_upd_rsp.empty() && !rxSar2rxEng_upd_rsp.empty()
						&& !(fsm_txSarRequest && txSar2rxEng_upd_rsp.empty()))
		{
			fsm_state = LOAD;
			fsm_txSarRequest = false;
		}

		control_bits[0] = fsm_meta.meta.ack;
		control_bits[1] = fsm_meta.meta.syn;
		control_bits[2] = fsm_meta.meta.fin;
		control_bits[3] = fsm_meta.meta.rst;
		switch (control_bits)
		{
		case 1: //ACK
			//if (!rxSar2rxEng_upd_rsp.empty() && !stateTable2rxEng_upd_rsp.empty() && !txSar2rxEng_upd_rsp.empty())
			if (fsm_state == LOAD)
			{
				stateTable2rxEng_upd_rsp.read(tcpState);
				rxSar2rxEng_upd_rsp.read(rxSar);
				txSar2rxEng_upd_rsp.read(txSar);
				rxEng2timer_clearRetransmitTimer.write(rxRetransmitTimerUpdate(fsm_meta.sessionID, (fsm_meta.meta.ackNumb == txSar.nextByte)));
				if (tcpState == ESTABLISHED || tcpState == SYN_RECEIVED || tcpState == FIN_WAIT_1 || tcpState == CLOSING || tcpState == LAST_ACK)
				{
					// Check if new ACK arrived
					if (fsm_meta.meta.ackNumb == txSar.prevAck && txSar.prevAck != txSar.nextByte)
					{
						// Not new ACK increase counter only if it does not contain data
						if (fsm_meta.meta.length == 0)
						{
							txSar.count++;
						}
					}
					else
					{
						// Notify probeTimer about new ACK
						rxEng2timer_clearProbeTimer.write(fsm_meta.sessionID);
						// Check for SlowStart & Increase Congestion Window
						if (txSar.cong_window <= (txSar.slowstart_threshold-MSS))
						{
							txSar.cong_window += MSS;
						}
						else if (txSar.cong_window <= 0xF7FF)
						{
							txSar.cong_window += 365; //TODO replace by approx. of (MSS x MSS) / cong_window
						}
						txSar.count = 0;
						txSar.fastRetransmitted = false;
					}
					// TX SAR
					if ((txSar.prevAck <= fsm_meta.meta.ackNumb && fsm_meta.meta.ackNumb <= txSar.nextByte)
							|| ((txSar.prevAck <= fsm_meta.meta.ackNumb || fsm_meta.meta.ackNumb <= txSar.nextByte) && txSar.nextByte < txSar.prevAck))
					{
						rxEng2txSar_upd_req.write((rxTxSarQuery(fsm_meta.sessionID, fsm_meta.meta.ackNumb, fsm_meta.meta.winSize, txSar.cong_window, txSar.count, ((txSar.count == 3) || txSar.fastRetransmitted))));
					}

					// Check if packet contains payload
					if (fsm_meta.meta.length != 0)
					{
						ap_uint<32> newRecvd = fsm_meta.meta.seqNumb+fsm_meta.meta.length;
						// Second part makes sure that app pointer is not overtaken
#if !(RX_DDR_BYPASS)
						ap_uint<16> free_space = ((rxSar.appd - rxSar.recvd(15, 0)) - 1);
						// Check if segment in order and if enough free space is available
						if ((fsm_meta.meta.seqNumb == rxSar.recvd) && (free_space > fsm_meta.meta.length))
#else
						if ((fsm_meta.meta.seqNumb == rxSar.recvd) && ((rxbuffer_max_data_count - rxbuffer_data_count) > 375))
#endif
						{
							rxEng2rxSar_upd_req.write(rxSarRecvd(fsm_meta.sessionID, newRecvd, 1));
							// Build memory address
							ap_uint<32> pkgAddr;
							pkgAddr(31, 30) = 0x0;
							pkgAddr(29, 16) = fsm_meta.sessionID(13, 0);
							pkgAddr(15, 0) = fsm_meta.meta.seqNumb(15, 0);
#if !(RX_DDR_BYPASS)
							rxBufferWriteCmd.write(mmCmd(pkgAddr, fsm_meta.meta.length));
#endif
							// Only notify about  new data available
							rxEng2rxApp_notification.write(appNotification(fsm_meta.sessionID, fsm_meta.meta.length, fsm_meta.srcIpAddress, fsm_meta.dstIpPort));
							dropDataFifoOut.write(false);
						}
						else
						{
							dropDataFifoOut.write(true);
						}

						// Sent ACK
						//rxEng2eventEng_setEvent.write(event(ACK, fsm_meta.sessionID));
					}
#if FAST_RETRANSMIT
					if (txSar.count == 3 && !txSar.fastRetransmitted)
					{
						rxEng2eventEng_setEvent.write(event(RT, fsm_meta.sessionID));
					}
					else if (fsm_meta.meta.length != 0)
#else
					if (fsm_meta.meta.length != 0)
#endif
					{
						rxEng2eventEng_setEvent.write(event(ACK, fsm_meta.sessionID));
					}


					// Reset Retransmit Timer
					//rxEng2timer_clearRetransmitTimer.write(rxRetransmitTimerUpdate(fsm_meta.sessionID, (mh_meta.ackNumb == txSarNextByte)));
					if (fsm_meta.meta.ackNumb == txSar.nextByte)
					{
						switch (tcpState)
						{
						case SYN_RECEIVED:
							rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, ESTABLISHED, 1)); //TODO MAYBE REARRANGE
							break;
						case CLOSING:
							rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, TIME_WAIT, 1));
							rxEng2timer_setCloseTimer.write(fsm_meta.sessionID);
							break;
						case LAST_ACK:
							rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, CLOSED, 1));
							break;
						default:
							rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, tcpState, 1));
							break;
						}
					}
					else //we have to release the lock
					{
						//reset rtTimer
						//rtTimer.write(rxRetransmitTimerUpdate(fsm_meta.sessionID));
						rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, tcpState, 1)); // or ESTABLISHED
					}
				} //end state if
				// TODO if timewait just send ACK, can it be time wait??
				else // state == (CLOSED || SYN_SENT || CLOSE_WAIT || FIN_WAIT_2 || TIME_WAIT)
				{
					// SENT RST, RFC 793: fig.11
					rxEng2eventEng_setEvent.write(rstEvent(fsm_meta.sessionID, fsm_meta.meta.seqNumb+fsm_meta.meta.length)); // noACK ?
					// if data is in the pipe it needs to be droppped
					if (fsm_meta.meta.length != 0)
					{
						dropDataFifoOut.write(true);
					}
					rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, tcpState, 1));
				}
				//fsm_state = LOAD;
			}
			break;
		case 2: //SYN
			//if (!stateTable2rxEng_upd_rsp.empty())
			if (fsm_state == LOAD)
			{
				stateTable2rxEng_upd_rsp.read(tcpState);
				rxSar2rxEng_upd_rsp.read(rxSar);
				if (tcpState == CLOSED || tcpState == SYN_SENT) // Actually this is LISTEN || SYN_SENT
				{
					// Initialize rxSar, SEQ + phantom byte, last '1' for makes sure appd is initialized
					rxEng2rxSar_upd_req.write(rxSarRecvd(fsm_meta.sessionID, fsm_meta.meta.seqNumb+1, 1, 1));
					// Initialize receive window
					rxEng2txSar_upd_req.write((rxTxSarQuery(fsm_meta.sessionID, 0, fsm_meta.meta.winSize, txSar.cong_window, 0, false))); //TODO maybe include count check
					// Set SYN_ACK event
					rxEng2eventEng_setEvent.write(event(SYN_ACK, fsm_meta.sessionID));
					// Change State to SYN_RECEIVED
					rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, SYN_RECEIVED, 1));
				}
				else if (tcpState == SYN_RECEIVED)// && mh_meta.seqNumb+1 == rxSar.recvd) // Maybe Check for seq
				{
					// If it is the same SYN, we resent SYN-ACK, almost like quick RT, we could also wait for RT timer
					if (fsm_meta.meta.seqNumb+1 == rxSar.recvd)
					{
						// Retransmit SYN_ACK
						rxEng2eventEng_setEvent.write(event(SYN_ACK, fsm_meta.sessionID, 1));
						rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, tcpState, 1));
					}
					else // Sent RST, RFC 793: fig.9 (old) duplicate SYN(+ACK)
					{
						rxEng2eventEng_setEvent.write(rstEvent(fsm_meta.sessionID, fsm_meta.meta.seqNumb+1)); //length == 0
						rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, CLOSED, 1));
					}
				}
				else // Any synchronized state
				{
					// Unexpected SYN arrived, reply with normal ACK, RFC 793: fig.10
					rxEng2eventEng_setEvent.write(event(ACK_NODELAY, fsm_meta.sessionID));
					// TODo send RST, has no ACK??
					// Respond with RST, no ACK, seq ==
					//eventEngine.write(rstEvent(mh_meta.seqNumb, mh_meta.length, true));
					rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, tcpState, 1));
				}
			}
			break;
		case 3: //SYN_ACK
			//if (!stateTable2rxEng_upd_rsp.empty() && !txSar2rxEng_upd_rsp.empty())
			if (fsm_state == LOAD)
			{
				stateTable2rxEng_upd_rsp.read(tcpState);
				rxSar2rxEng_upd_rsp.read(rxSar);
				txSar2rxEng_upd_rsp.read(txSar);
				rxEng2timer_clearRetransmitTimer.write(rxRetransmitTimerUpdate(fsm_meta.sessionID, (fsm_meta.meta.ackNumb == txSar.nextByte)));
				if ((tcpState == SYN_SENT) && (fsm_meta.meta.ackNumb == txSar.nextByte))// && !mh_lup.created)
				{
					//initialize rx_sar, SEQ + phantom byte, last '1' for appd init
					rxEng2rxSar_upd_req.write(rxSarRecvd(fsm_meta.sessionID, fsm_meta.meta.seqNumb+1, 1, 1));

					rxEng2txSar_upd_req.write((rxTxSarQuery(fsm_meta.sessionID, fsm_meta.meta.ackNumb, fsm_meta.meta.winSize, txSar.cong_window, 0, false))); //TODO maybe include count check

					// set ACK event
					rxEng2eventEng_setEvent.write(event(ACK_NODELAY, fsm_meta.sessionID));

					rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, ESTABLISHED, 1));
					openConStatusOut.write(openStatus(fsm_meta.sessionID, true));
				}
				else if (tcpState == SYN_SENT) //TODO correct answer?
				{
					// Sent RST, RFC 793: fig.9 (old) duplicate SYN(+ACK)
					rxEng2eventEng_setEvent.write(rstEvent(fsm_meta.sessionID, fsm_meta.meta.seqNumb+fsm_meta.meta.length+1));
					rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, CLOSED, 1));
				}
				else
				{
					// Unexpected SYN arrived, reply with normal ACK, RFC 793: fig.10
					rxEng2eventEng_setEvent.write(event(ACK_NODELAY, fsm_meta.sessionID));
					rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, tcpState, 1));
				}
			}
			break;
		case 5: //FIN (_ACK)
			//if (!rxSar2rxEng_upd_rsp.empty() && !stateTable2rxEng_upd_rsp.empty() && !txSar2rxEng_upd_rsp.empty())
			if (fsm_state == LOAD)
			{
				stateTable2rxEng_upd_rsp.read(tcpState);
				rxSar2rxEng_upd_rsp.read(rxSar);
				txSar2rxEng_upd_rsp.read(txSar);
				rxEng2timer_clearRetransmitTimer.write(rxRetransmitTimerUpdate(fsm_meta.sessionID, (fsm_meta.meta.ackNumb == txSar.nextByte)));
				// Check state and if FIN in order, Current out of order FINs are not accepted
				if ((tcpState == ESTABLISHED || tcpState == FIN_WAIT_1 || tcpState == FIN_WAIT_2) && (rxSar.recvd == fsm_meta.meta.seqNumb))
				{
					rxEng2txSar_upd_req.write((rxTxSarQuery(fsm_meta.sessionID, fsm_meta.meta.ackNumb, fsm_meta.meta.winSize, txSar.cong_window, txSar.count, txSar.fastRetransmitted))); //TODO include count check

					// +1 for phantom byte, there might be data too
					rxEng2rxSar_upd_req.write(rxSarRecvd(fsm_meta.sessionID, fsm_meta.meta.seqNumb+fsm_meta.meta.length+1, 1)); //diff to ACK

					// Clear the probe timer
					rxEng2timer_clearProbeTimer.write(fsm_meta.sessionID);

					// Check if there is payload
					if (fsm_meta.meta.length != 0)
					{
						ap_uint<32> pkgAddr;
						pkgAddr(31, 30) = 0x0;
						pkgAddr(29, 16) = fsm_meta.sessionID(13, 0);
						pkgAddr(15, 0) = fsm_meta.meta.seqNumb(15, 0);
#if !(RX_DDR_BYPASS)
						rxBufferWriteCmd.write(mmCmd(pkgAddr, fsm_meta.meta.length));
#endif
						// Tell Application new data is available and connection got closed
						rxEng2rxApp_notification.write(appNotification(fsm_meta.sessionID, fsm_meta.meta.length, fsm_meta.srcIpAddress, fsm_meta.dstIpPort, true));
						dropDataFifoOut.write(false);
					}
					else if (tcpState == ESTABLISHED)
					{
						// Tell Application connection got closed
						rxEng2rxApp_notification.write(appNotification(fsm_meta.sessionID, fsm_meta.srcIpAddress, fsm_meta.dstIpPort, true)); //CLOSE
					}

					// Update state
					if (tcpState == ESTABLISHED)
					{
						rxEng2eventEng_setEvent.write(event(FIN, fsm_meta.sessionID));
						rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, LAST_ACK, 1));
					}
					else //FIN_WAIT_1 || FIN_WAIT_2
					{
						if (fsm_meta.meta.ackNumb == txSar.nextByte) //check if final FIN is ACK'd -> LAST_ACK
						{
							rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, TIME_WAIT, 1));
							rxEng2timer_setCloseTimer.write(fsm_meta.sessionID);
						}
						else
						{
							rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, CLOSING, 1));
						}
						rxEng2eventEng_setEvent.write(event(ACK, fsm_meta.sessionID));
					}
				}
				else // NOT (ESTABLISHED || FIN_WAIT_1 || FIN_WAIT_2)
				{
					rxEng2eventEng_setEvent.write(event(ACK, fsm_meta.sessionID));
					rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, tcpState, 1));
					// If there is payload we need to drop it
					if (fsm_meta.meta.length != 0)
					{
						dropDataFifoOut.write(true);
					}
				}
			}
			break;
		default: //TODO MAYBE load everthing all the time
			// stateTable is locked, make sure it is released in at the end
			// If there is an ACK we read txSar
			// We always read rxSar
			if (fsm_state == LOAD)
			{
				stateTable2rxEng_upd_rsp.read(tcpState);
				rxSar2rxEng_upd_rsp.read(rxSar); //TODO not sure nb works
				txSar2rxEng_upd_rsp.read_nb(txSar);
			}
			if (fsm_state == LOAD)
			{
				// Handle if RST
				if (fsm_meta.meta.rst)
				{
					if (tcpState == SYN_SENT) //TODO this would be a RST,ACK i think
					{
						if (fsm_meta.meta.ackNumb == txSar.nextByte) // Check if matching SYN
						{
							//tell application, could not open connection
							openConStatusOut.write(openStatus(fsm_meta.sessionID, false));
							rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, CLOSED, 1));
							rxEng2timer_clearRetransmitTimer.write(rxRetransmitTimerUpdate(fsm_meta.sessionID, true));
						}
						else
						{
							// Ignore since not matching
							rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, tcpState, 1));
						}
					}
					else
					{
						// Check if in window
						if (fsm_meta.meta.seqNumb == rxSar.recvd)
						{
							//tell application, RST occurred, abort
							rxEng2rxApp_notification.write(appNotification(fsm_meta.sessionID, fsm_meta.srcIpAddress, fsm_meta.dstIpPort, true)); //RESET
							rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, CLOSED, 1)); //TODO maybe some TIME_WAIT state
							rxEng2timer_clearRetransmitTimer.write(rxRetransmitTimerUpdate(fsm_meta.sessionID, true));
						}
						else
						{
							// Ingore since not matching window
							rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, tcpState, 1));
						}
					}
				}
				else // Handle non RST bogus packages
				{
					//TODO maybe sent RST ourselves, or simply ignore
					// For now ignore, sent ACK??
					//eventsOut.write(rstEvent(mh_meta.seqNumb, 0, true));
					rxEng2stateTable_upd_req.write(stateQuery(fsm_meta.sessionID, tcpState, 1));
				} // if rst
			} // if fsm_stat
			break;
		} //switch control_bits
		break;
	} //switch state
}

/** @ingroup rx_engine
 *	Drops packets if their metadata did not match / are invalid, as indicated by @param dropBuffer
 *	@param[in]		dataIn, incoming data stream
 *	@param[in]		dropFifoIn, Drop-FIFO indicating if packet needs to be dropped
 *	@param[out]		rxBufferDataOut, outgoing data stream
 */
void rxPackageDropper(stream<axiWord>&		dataIn,
					  stream<bool>&			dropFifoIn1,
					  stream<bool>&			dropFifoIn2,
					  stream<axiWord>&		rxBufferDataOut) {
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	enum tpfStateType {READ_DROP1, READ_DROP2, FWD, DROP};
	static tpfStateType tpf_state = READ_DROP1;

	bool drop;

	switch (tpf_state) {
	case READ_DROP1: //Drop1
		if (!dropFifoIn1.empty())
		{
			dropFifoIn1.read(drop);
			if (drop)
			{
				tpf_state = DROP;
			}
			else
			{
				tpf_state = READ_DROP2;
			}
		}
		break;
	case READ_DROP2:
		if (!dropFifoIn2.empty())
		{
			dropFifoIn2.read(drop);
			if (drop)
			{
				tpf_state = DROP;
			}
			else
			{
				tpf_state = FWD;
			}
		}
		break;
	case FWD:
		if(!dataIn.empty() && !rxBufferDataOut.full()) {
			axiWord currWord = dataIn.read();
			if (currWord.last)
			{
				tpf_state = READ_DROP1;
			}
			rxBufferDataOut.write(currWord);
		}
		break;
	case DROP:
		if(!dataIn.empty()) {
			axiWord currWord = dataIn.read();
			if (currWord.last)
			{
				tpf_state = READ_DROP1;
			}
		}
		break;
	} // switch
}

/** @ingroup rx_engine
 *  Delays the notifications to the application until the data is actually is written to memory
 *  @param[in]		rxWriteStatusIn, the status which we get back from the DATA MOVER it indicates if the write was successful
 *  @param[in]		internalNotificationFifoIn, incoming notifications
 *  @param[out]		notificationOut, outgoing notifications
 *  @TODO Handle unsuccessful write to memory
 */
void rxAppNotificationDelayer(	stream<mmStatus>&				rxWriteStatusIn, stream<appNotification>&		internalNotificationFifoIn,
								stream<appNotification>&		notificationOut, stream<ap_uint<1> > &doubleAccess) {
#pragma HLS INLINE off
#pragma HLS pipeline II=1

	static stream<appNotification> rand_notificationBuffer("rand_notificationBuffer");
	#pragma HLS STREAM variable=rand_notificationBuffer depth=32 //depends on memory delay
	#pragma HLS DATA_PACK variable=rand_notificationBuffer

	static ap_uint<1>		rxAppNotificationDoubleAccessFlag = false;
	static ap_uint<5>		rand_fifoCount = 0;
	static mmStatus			rxAppNotificationStatus1, rxAppNotificationStatus2;
	static appNotification	rxAppNotification;

	if (rxAppNotificationDoubleAccessFlag == true) {
		if(!rxWriteStatusIn.empty()) {
			rxWriteStatusIn.read(rxAppNotificationStatus2);
			rand_fifoCount--;
			if (rxAppNotificationStatus1.okay && rxAppNotificationStatus2.okay)
				notificationOut.write(rxAppNotification);
			rxAppNotificationDoubleAccessFlag = false;
		}
	}
	else if (rxAppNotificationDoubleAccessFlag == false) {
		if(!rxWriteStatusIn.empty() && !rand_notificationBuffer.empty() && !doubleAccess.empty()) {
			rxWriteStatusIn.read(rxAppNotificationStatus1);
			rand_notificationBuffer.read(rxAppNotification);
			rxAppNotificationDoubleAccessFlag = doubleAccess.read(); 	// Read the double notification flag. If one then go and w8 for the second status
			if (rxAppNotificationDoubleAccessFlag == 0) {				// if the memory access was not broken down in two for this segment
				rand_fifoCount--;
				if (rxAppNotificationStatus1.okay)
					notificationOut.write(rxAppNotification);				// Output the notification
			}
			//TODO else, we are screwed since the ACK is already sent
		}
		else if (!internalNotificationFifoIn.empty() && (rand_fifoCount < 31)) {
			internalNotificationFifoIn.read(rxAppNotification);
			if (rxAppNotification.length != 0) {
				rand_notificationBuffer.write(rxAppNotification);
				rand_fifoCount++;
			}
			else
				notificationOut.write(rxAppNotification);
		}
	}
}

void rxEventMerger(stream<extendedEvent>& in1, stream<event>& in2, stream<extendedEvent>& out)
{
	#pragma HLS PIPELINE II=1
	#pragma HLS INLINE

	if (!in1.empty())
	{
		out.write(in1.read());
	}
	else if (!in2.empty())
	{
		out.write(in2.read());
	}
}

void rxEngMemWrite(	stream<axiWord>& 				rxMemWrDataIn, stream<mmCmd>&				rxMemWrCmdIn,
					stream<mmCmd>&					rxMemWrCmdOut, stream<axiWord>&				rxMemWrDataOut,
					stream<ap_uint<1> >&			doubleAccess) {
#pragma HLS pipeline II=1
#pragma HLS INLINE off

	static enum rxmwrState{RXMEMWR_IDLE, RXMEMWR_WRFIRST, RXMEMWR_EVALSECOND, RXMEMWR_WRSECOND, RXMEMWR_WRSECONDSTR, RXMEMWR_ALIGNED, RXMEMWR_RESIDUE} rxMemWrState;
	static mmCmd rxMemWriterCmd = mmCmd(0, 0);
	static ap_uint<16> rxEngBreakTemp = 0;
	static uint8_t lengthBuffer = 0;
	static ap_uint<3> rxEngAccessResidue = 0;
	static bool txAppBreakdown = false;
	static axiWord pushWord = axiWord(0, 0xFF, 0);


	switch (rxMemWrState) {
	case RXMEMWR_IDLE:
		if (!rxMemWrCmdIn.empty() && !rxMemWrCmdOut.full() && !doubleAccess.full()) {
			rxMemWriterCmd = rxMemWrCmdIn.read();
			mmCmd tempCmd = rxMemWriterCmd;
			if ((rxMemWriterCmd.saddr.range(15, 0) + rxMemWriterCmd.bbt) > 65536) {
				rxEngBreakTemp = 65536 - rxMemWriterCmd.saddr;
				rxMemWriterCmd.bbt -= rxEngBreakTemp;
				tempCmd = mmCmd(rxMemWriterCmd.saddr, rxEngBreakTemp);
				txAppBreakdown = true;
			}
			else
			{
				rxEngBreakTemp = rxMemWriterCmd.bbt;
			}
			rxMemWrCmdOut.write(tempCmd);
			doubleAccess.write(txAppBreakdown);
			//txAppPktCounter++;
			//std::cerr <<  "Cmd: " << std::dec << txAppPktCounter << " - " << std::hex << tempCmd.saddr << " - " << tempCmd.bbt << std::endl;
			rxMemWrState = RXMEMWR_WRFIRST;
		}
		break;
	case RXMEMWR_WRFIRST:
		if (!rxMemWrDataIn.empty() && !rxMemWrDataOut.full()) {
			rxMemWrDataIn.read(pushWord);
			axiWord outputWord = pushWord;
			ap_uint<4> byteCount = keepToLen(pushWord.keep);
			if (rxEngBreakTemp > 8)
			{
				rxEngBreakTemp -= 8;
			}
			else
			{
				if (txAppBreakdown == true)
				{				/// Changes are to go in here
					if (rxMemWriterCmd.saddr.range(15, 0) % 8 != 0) // If the word is not perfectly aligned then there is some magic to be worked.
					{
						outputWord.keep = lenToKeep(rxEngBreakTemp);
					}
					outputWord.last = 1;
					rxMemWrState = RXMEMWR_EVALSECOND;
					rxEngAccessResidue = byteCount - rxEngBreakTemp;
					lengthBuffer = rxEngBreakTemp;	// Buffer the number of bits consumed.
				}
				else
				{
					rxMemWrState = RXMEMWR_IDLE;
				}
			}
			//txAppWordCounter++;
			//std::cerr <<  std::dec << cycleCounter << " - " << txAppWordCounter << " - " << std::hex << pushWord.data << std::endl;
			rxMemWrDataOut.write(outputWord);
		}
		break;
	case RXMEMWR_EVALSECOND:
		if (!rxMemWrCmdOut.full()) {
			if (rxMemWriterCmd.saddr.range(15, 0) % 8 == 0)
				rxMemWrState = RXMEMWR_ALIGNED;
			//else if (rxMemWriterCmd.bbt + rxEngAccessResidue > 8 || rxEngAccessResidue > 0)
			else if (rxMemWriterCmd.bbt - rxEngAccessResidue > 0)
				rxMemWrState = RXMEMWR_WRSECONDSTR;
			else
				rxMemWrState = RXMEMWR_RESIDUE;
			rxMemWriterCmd.saddr.range(15, 0) = 0;
			rxEngBreakTemp = rxMemWriterCmd.bbt;
			rxMemWrCmdOut.write(mmCmd(rxMemWriterCmd.saddr, rxEngBreakTemp));
			//std::cerr <<  "Cmd: " << std::dec << txAppPktCounter << " - " << std::hex << txAppTempCmd.saddr << " - " << txAppTempCmd.bbt << std::endl;
			txAppBreakdown = false;

		}
		break;
	case RXMEMWR_ALIGNED:	// This is the non-realignment state
		if (!rxMemWrDataIn.empty() & !rxMemWrDataOut.full()) {
			rxMemWrDataIn.read(pushWord);
			rxMemWrDataOut.write(pushWord);
			if (pushWord.last == 1)
				rxMemWrState = RXMEMWR_IDLE;
		}
		break;
	case RXMEMWR_WRSECONDSTR: // We go into this state when we need to realign things
		if (!rxMemWrDataIn.empty() && !rxMemWrDataOut.full()) {
			axiWord outputWord = axiWord(0, 0xFF, 0);
			outputWord.data.range(((8-lengthBuffer)*8) - 1, 0) = pushWord.data.range(63, lengthBuffer*8);
			pushWord = rxMemWrDataIn.read();
			outputWord.data.range(63, (8-lengthBuffer)*8) = pushWord.data.range((lengthBuffer * 8), 0 );

			if (pushWord.last == 1) {
				if (rxEngBreakTemp - rxEngAccessResidue > lengthBuffer)	{ // In this case there's residue to be handled
					rxEngBreakTemp -= 8;
					rxMemWrState = RXMEMWR_RESIDUE;
				}
				else {
					outputWord.keep = lenToKeep(rxEngBreakTemp);
					outputWord.last = 1;
					rxMemWrState = RXMEMWR_IDLE;
				}
			}
			else
				rxEngBreakTemp -= 8;
			rxMemWrDataOut.write(outputWord);
		}
		break;
	case RXMEMWR_RESIDUE:
		if (!rxMemWrDataOut.full()) {
			axiWord outputWord = axiWord(0, lenToKeep(rxEngBreakTemp), 1);
			outputWord.data.range(((8-lengthBuffer)*8) - 1, 0) = pushWord.data.range(63, lengthBuffer*8);
			rxMemWrDataOut.write(outputWord);
			rxMemWrState = RXMEMWR_IDLE;
		}
		break;
	} //switch
}

/** @ingroup rx_engine
 *  The @ref rx_engine is processing the data packets on the receiving path.
 *  When a new packet enters the engine its TCP checksum is tested, afterwards the header is parsed
 *  and some more checks are done. Before it is evaluated by the main TCP state machine which triggers Events
 *  and updates the data structures depending on the packet. If the packet contains valid payload it is stored
 *  in memory and the application is notified about the new data.
 *  @param[in]		ipRxData
 *  @param[in]		sLookup2rxEng_rsp
 *  @param[in]		stateTable2rxEng_upd_rsp
 *  @param[in]		portTable2rxEng_rsp
 *  @param[in]		rxSar2rxEng_upd_rsp
 *  @param[in]		txSar2rxEng_upd_rsp
 *  @param[in]		rxBufferWriteStatus
 *
 *  @param[out]		rxBufferWriteData
 *  @param[out]		rxEng2sLookup_req
 *  @param[out]		rxEng2stateTable_upd_req
 *  @param[out]		rxEng2portTable_req
 *  @param[out]		rxEng2rxSar_upd_req
 *  @param[out]		rxEng2txSar_upd_req
 *  @param[out]		rxEng2timer_clearRetransmitTimer
 *  @param[out]		rxEng2timer_setCloseTimer
 *  @param[out]		openConStatusOut
 *  @param[out]		rxEng2eventEng_setEvent
 *  @param[out]		rxBufferWriteCmd
 *  @param[out]		rxEng2rxApp_notification
 */
void rx_engine(	stream<axiWord>&					ipRxData,
				stream<sessionLookupReply>&			sLookup2rxEng_rsp,
				stream<sessionState>&				stateTable2rxEng_upd_rsp,
				stream<bool>&						portTable2rxEng_rsp,
				stream<rxSarEntry>&					rxSar2rxEng_upd_rsp,
				stream<rxTxSarReply>&				txSar2rxEng_upd_rsp,
#if !(RX_DDR_BYPASS)
				stream<mmStatus>&					rxBufferWriteStatus,
#endif
				stream<axiWord>&					rxBufferWriteData,
				stream<sessionLookupQuery>&			rxEng2sLookup_req,
				stream<stateQuery>&					rxEng2stateTable_upd_req,
				stream<ap_uint<16> >&				rxEng2portTable_req,
				stream<rxSarRecvd>&					rxEng2rxSar_upd_req,
				stream<rxTxSarQuery>&				rxEng2txSar_upd_req,
				stream<rxRetransmitTimerUpdate>&	rxEng2timer_clearRetransmitTimer,
				stream<ap_uint<16> >&				rxEng2timer_clearProbeTimer,
				stream<ap_uint<16> >&				rxEng2timer_setCloseTimer,
				stream<openStatus>&					openConStatusOut,
				stream<extendedEvent>&				rxEng2eventEng_setEvent,
#if !(RX_DDR_BYPASS)
				stream<mmCmd>&						rxBufferWriteCmd,
				stream<appNotification>&			rxEng2rxApp_notification)
#else
				stream<appNotification>&			rxEng2rxApp_notification,
				ap_uint<32>					rxbuffer_data_count,
				ap_uint<32>					rxbuffer_max_data_count)
#endif
{
//#pragma HLS DATAFLOW
//#pragma HLS INTERFACE ap_ctrl_none port=return
#pragma HLS INLINE

	// Axi Streams
	static stream<axiWord>		rxEng_dataBuffer0("rxEng_dataBuffer0");
	static stream<axiWord>		rxEng_dataBuffer1("rxEng_dataBuffer1");
	static stream<axiWord>		rxEng_dataBuffer2("rxEng_dataBuffer2");
	static stream<axiWord>		rxEng_dataBuffer3("rxEng_dataBuffer3");
	#pragma HLS stream variable=rxEng_dataBuffer0 depth=8
	#pragma HLS stream variable=rxEng_dataBuffer1 depth=8
	#pragma HLS stream variable=rxEng_dataBuffer2 depth=256 //critical, tcp checksum computation
	#pragma HLS stream variable=rxEng_dataBuffer3 depth=8
	#pragma HLS DATA_PACK variable=rxEng_dataBuffer0
	#pragma HLS DATA_PACK variable=rxEng_dataBuffer1
	#pragma HLS DATA_PACK variable=rxEng_dataBuffer2
	#pragma HLS DATA_PACK variable=rxEng_dataBuffer3

	// Meta Streams/FIFOs
	static stream<bool>					rxEng_tcpValidFifo("rx_tcpValidFifo");
	static stream<rxEngineMetaData>		rxEng_metaDataFifo("rx_metaDataFifo");
	static stream<rxFsmMetaData>		rxEng_fsmMetaDataFifo("rxEng_fsmMetaDataFifo");
	static stream<fourTuple>			rxEng_tupleBuffer("rx_tupleBuffer");
	static stream<ap_uint<16> >			rxEng_tcpLenFifo("rx_tcpLenFifo");
	#pragma HLS stream variable=rxEng_tcpValidFifo depth=2
	#pragma HLS stream variable=rxEng_metaDataFifo depth=2
	#pragma HLS stream variable=rxEng_tupleBuffer depth=2
	#pragma HLS stream variable=rxEng_tcpLenFifo depth=2
	#pragma HLS DATA_PACK variable=rxEng_metaDataFifo
	#pragma HLS DATA_PACK variable=rxEng_tupleBuffer

	static stream<extendedEvent>		rxEng_metaHandlerEventFifo("rxEng_metaHandlerEventFifo");
	static stream<event>				rxEng_fsmEventFifo("rxEng_fsmEventFifo");
	#pragma HLS stream variable=rxEng_metaHandlerEventFifo depth=2
	#pragma HLS stream variable=rxEng_fsmEventFifo depth=2
	#pragma HLS DATA_PACK variable=rxEng_metaHandlerEventFifo
	#pragma HLS DATA_PACK variable=rxEng_fsmEventFifo

	static stream<bool>					rxEng_metaHandlerDropFifo("rxEng_metaHandlerDropFifo");
	static stream<bool>					rxEng_fsmDropFifo("rxEng_fsmDropFifo");
	#pragma HLS stream variable=rxEng_metaHandlerDropFifo depth=2
	#pragma HLS stream variable=rxEng_fsmDropFifo depth=2
	#pragma HLS DATA_PACK variable=rxEng_metaHandlerDropFifo
	#pragma HLS DATA_PACK variable=rxEng_fsmDropFifo

	static stream<appNotification> rx_internalNotificationFifo("rx_internalNotificationFifo");
	#pragma HLS stream variable=rx_internalNotificationFifo depth=8 //This depends on the memory delay
	#pragma HLS DATA_PACK variable=rx_internalNotificationFifo

	static stream<mmCmd> 					rxTcpFsm2wrAccessBreakdown("rxTcpFsm2wrAccessBreakdown");
	#pragma HLS stream variable=rxTcpFsm2wrAccessBreakdown depth=8
	#pragma HLS DATA_PACK variable=rxTcpFsm2wrAccessBreakdown

	static stream<axiWord> 					rxPkgDrop2rxMemWriter("rxPkgDrop2rxMemWriter");
	#pragma HLS stream variable=rxPkgDrop2rxMemWriter depth=16
	#pragma HLS DATA_PACK variable=rxPkgDrop2rxMemWriter

	static stream<ap_uint<1> >				rxEngDoubleAccess("rxEngDoubleAccess");
	#pragma HLS stream variable=rxEngDoubleAccess depth=8
	rxTcpLengthExtract(ipRxData, rxEng_dataBuffer0, rxEng_tcpLenFifo);

	rxInsertPseudoHeader(rxEng_dataBuffer0, rxEng_tcpLenFifo, rxEng_dataBuffer1);

	rxCheckTCPchecksum(rxEng_dataBuffer1, rxEng_dataBuffer2, rxEng_tcpValidFifo, rxEng_metaDataFifo,
						rxEng_tupleBuffer, rxEng2portTable_req);

	rxTcpInvalidDropper(rxEng_dataBuffer2, rxEng_tcpValidFifo, rxEng_dataBuffer3);

	rxMetadataHandler(	rxEng_metaDataFifo,
						sLookup2rxEng_rsp,
						portTable2rxEng_rsp,
						rxEng_tupleBuffer,
						rxEng2sLookup_req,
						rxEng_metaHandlerEventFifo,
						rxEng_metaHandlerDropFifo,
						rxEng_fsmMetaDataFifo);

	rxTcpFSM(			rxEng_fsmMetaDataFifo,
							stateTable2rxEng_upd_rsp,
							rxSar2rxEng_upd_rsp,
							txSar2rxEng_upd_rsp,
							rxEng2stateTable_upd_req,
							rxEng2rxSar_upd_req,
							rxEng2txSar_upd_req,
							rxEng2timer_clearRetransmitTimer,
							rxEng2timer_clearProbeTimer,
							rxEng2timer_setCloseTimer,
							openConStatusOut,
							rxEng_fsmEventFifo,
							rxEng_fsmDropFifo,
#if !(RX_DDR_BYPASS)
							rxTcpFsm2wrAccessBreakdown,
							rx_internalNotificationFifo);
#else
							rxEng2rxApp_notification,
							rxbuffer_data_count,
							rxbuffer_max_data_count);
#endif

#if !(RX_DDR_BYPASS)
	rxPackageDropper(rxEng_dataBuffer3, rxEng_metaHandlerDropFifo, rxEng_fsmDropFifo, rxPkgDrop2rxMemWriter);

	rxEngMemWrite(rxPkgDrop2rxMemWriter, rxTcpFsm2wrAccessBreakdown, rxBufferWriteCmd, rxBufferWriteData,rxEngDoubleAccess);

	rxAppNotificationDelayer(rxBufferWriteStatus, rx_internalNotificationFifo, rxEng2rxApp_notification, rxEngDoubleAccess);
#else
	rxPackageDropper(rxEng_dataBuffer3, rxEng_metaHandlerDropFifo, rxEng_fsmDropFifo, rxBufferWriteData);
#endif
	rxEventMerger(rxEng_metaHandlerEventFifo, rxEng_fsmEventFifo, rxEng2eventEng_setEvent);

}

