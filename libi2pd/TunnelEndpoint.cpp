/*
* Copyright (c) 2013-2020, The PurpleI2P Project
*
* This file is part of Purple i2pd project and licensed under BSD3
*
* See full license text in LICENSE file at top of project tree
*/

#include "I2PEndian.h"
#include <string.h>
#include "Crypto.h"
#include "Log.h"
#include "NetDb.hpp"
#include "I2NPProtocol.h"
#include "Transports.h"
#include "RouterContext.h"
#include "Timestamp.h"
#include "TunnelEndpoint.h"

namespace i2p
{
namespace tunnel
{
	TunnelEndpoint::~TunnelEndpoint ()
	{
	}

	void TunnelEndpoint::HandleDecryptedTunnelDataMsg (std::shared_ptr<I2NPMessage> msg)
	{
		m_NumReceivedBytes += TUNNEL_DATA_MSG_SIZE;

		uint8_t * decrypted = msg->GetPayload () + 20; // 4 + 16
		uint8_t * zero = (uint8_t *)memchr (decrypted + 4, 0, TUNNEL_DATA_ENCRYPTED_SIZE - 4); // witout 4-byte checksum
		if (zero)
		{
			uint8_t * fragment = zero + 1;
			// verify checksum
			memcpy (msg->GetPayload () + TUNNEL_DATA_MSG_SIZE, msg->GetPayload () + 4, 16); // copy iv to the end
			uint8_t hash[32];
			SHA256(fragment, TUNNEL_DATA_MSG_SIZE -(fragment - msg->GetPayload ()) + 16, hash); // payload + iv
			if (memcmp (hash, decrypted, 4))
			{
				LogPrint (eLogError, "TunnelMessage: checksum verification failed");
				return;
			}
			// process fragments
			while (fragment < decrypted + TUNNEL_DATA_ENCRYPTED_SIZE)
			{
				uint8_t flag = fragment[0];
				fragment++;

				bool isFollowOnFragment = flag & 0x80, isLastFragment = true;
				uint32_t msgID = 0;
				int fragmentNum = 0;
				TunnelMessageBlockEx& m = m_CurrentMessage;
				if (!isFollowOnFragment)
				{
					// first fragment
					if (m_CurrentMsgID)
						AddIncompleteCurrentMessage (); // we have got a new message while previous is not complete
					
					m.deliveryType = (TunnelDeliveryType)((flag >> 5) & 0x03);
					switch (m.deliveryType)
					{
						case eDeliveryTypeLocal: // 0
						break;
						case eDeliveryTypeTunnel: // 1
							m.tunnelID = bufbe32toh (fragment);
							fragment += 4; // tunnelID
							m.hash = i2p::data::IdentHash (fragment);
							fragment += 32; // hash
						break;
						case eDeliveryTypeRouter: // 2
							m.hash = i2p::data::IdentHash (fragment);
							fragment += 32; // to hash
						break;
						default: ;
					}

					bool isFragmented = flag & 0x08;
					if (isFragmented)
					{
						// Message ID
						msgID = bufbe32toh (fragment);
						fragment += 4;
						m_CurrentMsgID = msgID;
						isLastFragment = false;
					}
				}
				else
				{
					// follow on
					msgID = bufbe32toh (fragment); // MessageID
					fragment += 4;
					fragmentNum = (flag >> 1) & 0x3F; // 6 bits
					isLastFragment = flag & 0x01;
				}

				uint16_t size = bufbe16toh (fragment);
				fragment += 2;

				if (isFollowOnFragment && m_CurrentMsgID && m_CurrentMsgID == msgID && 
				    m_CurrentMessage.nextFragmentNum == fragmentNum)
				{	
					HandleCurrenMessageFollowOnFragment (fragment, size, isLastFragment);
					fragment += size;
					continue;
				}	
				
				msg->offset = fragment - msg->buf;
				msg->len = msg->offset + size;
				if (msg->len > msg->maxLen)
				{
					LogPrint (eLogError, "TunnelMessage: fragment is too long ", (int)size);
					m_CurrentMsgID = 0; m_CurrentMessage.data = nullptr;
					return;
				}
				if (fragment + size < decrypted + TUNNEL_DATA_ENCRYPTED_SIZE)
				{
					// this is not last message. we have to copy it
					m.data = NewI2NPTunnelMessage ();
					m.data->offset += TUNNEL_GATEWAY_HEADER_SIZE; // reserve room for TunnelGateway header
					m.data->len += TUNNEL_GATEWAY_HEADER_SIZE;
					*(m.data) = *msg;
				}
				else
					m.data = msg;

				if (!isFollowOnFragment)
				{	
					if (isLastFragment)
					{	
						HandleNextMessage (m);
						m_CurrentMsgID = 0; m_CurrentMessage.data = nullptr;
					}	
					else if (msgID)
					{
						m_CurrentMessage.nextFragmentNum = 1;
						m_CurrentMessage.receiveTime = i2p::util::GetMillisecondsSinceEpoch ();
						HandleOutOfSequenceFragments (msgID, m_CurrentMessage);
					}	
					else
					{	
						LogPrint (eLogError, "TunnelMessage: Message is fragmented, but msgID is not presented");
						m_CurrentMsgID = 0; m_CurrentMessage.data = nullptr;
					}	
				}	
				else
				{
					m.nextFragmentNum = fragmentNum;
					HandleFollowOnFragment (msgID, isLastFragment, m);
					m_CurrentMsgID = 0; m_CurrentMessage.data = nullptr;
				}	

				fragment += size;
			}
		}
		else
			LogPrint (eLogError, "TunnelMessage: zero not found");
	}

	void TunnelEndpoint::HandleFollowOnFragment (uint32_t msgID, bool isLastFragment, const TunnelMessageBlockEx& m)
	{
		auto fragment = m.data->GetBuffer ();
		auto size = m.data->GetLength ();
		auto it = m_IncompleteMessages.find (msgID);
		if (it != m_IncompleteMessages.end())
		{
			auto& msg = it->second;
			if (m.nextFragmentNum == msg.nextFragmentNum)
			{
				if (ConcatFollowOnFragment (msg, fragment, size))
				{
					if (isLastFragment)
					{
						// message complete
						HandleNextMessage (msg);
						m_IncompleteMessages.erase (it);
					}
					else
					{
						msg.nextFragmentNum++;
						HandleOutOfSequenceFragments (msgID, msg);
					}
				}
				else
				{
					LogPrint (eLogError, "TunnelMessage: Fragment ", m.nextFragmentNum, " of message ", msgID, "exceeds max I2NP message size, message dropped");
					m_IncompleteMessages.erase (it);
				}
			}
			else
			{
				LogPrint (eLogWarning, "TunnelMessage: Unexpected fragment ", (int)m.nextFragmentNum, " instead ", (int)msg.nextFragmentNum, " of message ", msgID, ", saved");
				AddOutOfSequenceFragment (msgID, m.nextFragmentNum, isLastFragment, m.data);
			}
		}
		else
		{
			LogPrint (eLogWarning, "TunnelMessage: First fragment of message ", msgID, " not found, saved");
			AddOutOfSequenceFragment (msgID, m.nextFragmentNum, isLastFragment, m.data);
		}
	}

	bool TunnelEndpoint::ConcatFollowOnFragment (TunnelMessageBlockEx& msg, const uint8_t * fragment, size_t size) const
	{
		if (msg.data->len + size < I2NP_MAX_MESSAGE_SIZE) // check if message is not too long
		{
			if (msg.data->len + size > msg.data->maxLen)
			{
			//	LogPrint (eLogWarning, "TunnelMessage: I2NP message size ", msg.data->maxLen, " is not enough");
				auto newMsg = NewI2NPMessage ();
				*newMsg = *(msg.data);
				msg.data = newMsg;
			}
			if (msg.data->Concat (fragment, size) < size) // concatenate fragment
			{	
				LogPrint (eLogError, "TunnelMessage: I2NP buffer overflow ", msg.data->maxLen);
				return false;
			}	
		}
		else
			return false;
		return true;
	}	

	void TunnelEndpoint::HandleCurrenMessageFollowOnFragment (const uint8_t * fragment, size_t size, bool isLastFragment)
	{
		if (ConcatFollowOnFragment (m_CurrentMessage, fragment, size))
		{
			if (isLastFragment)
			{
				// message complete
				HandleNextMessage (m_CurrentMessage);
				m_CurrentMsgID = 0; m_CurrentMessage.data = nullptr;
			}
			else
			{
				m_CurrentMessage.nextFragmentNum++;
				HandleOutOfSequenceFragments (m_CurrentMsgID, m_CurrentMessage);
			}
		}
		else
		{
			LogPrint (eLogError, "TunnelMessage: Fragment ", m_CurrentMessage.nextFragmentNum, " of message ", m_CurrentMsgID, " exceeds max I2NP message size, message dropped");
			m_CurrentMsgID = 0; m_CurrentMessage.data = nullptr;
		}
	}		

	void TunnelEndpoint::AddIncompleteCurrentMessage ()
	{
		if (m_CurrentMsgID)
		{
			auto ret = m_IncompleteMessages.emplace (m_CurrentMsgID, m_CurrentMessage);
			if (!ret.second)
				LogPrint (eLogError, "TunnelMessage: Incomplete message ", m_CurrentMsgID, " already exists");
			m_CurrentMessage.data = nullptr;
			m_CurrentMsgID = 0;
		}	
	}	
	
	void TunnelEndpoint::AddOutOfSequenceFragment (uint32_t msgID, uint8_t fragmentNum, bool isLastFragment, std::shared_ptr<I2NPMessage> data)
	{
		if (!m_OutOfSequenceFragments.insert ({(uint64_t)msgID << 32 | fragmentNum, 
			{isLastFragment, data, i2p::util::GetMillisecondsSinceEpoch () }}).second)
			LogPrint (eLogInfo, "TunnelMessage: duplicate out-of-sequence fragment ", fragmentNum, " of message ", msgID);
	}

	void TunnelEndpoint::HandleOutOfSequenceFragments (uint32_t msgID, TunnelMessageBlockEx& msg)
	{
		while (ConcatNextOutOfSequenceFragment (msgID, msg))
		{
			if (!msg.nextFragmentNum) // message complete
			{
				HandleNextMessage (msg);
				if (&msg == &m_CurrentMessage)
				{
					m_CurrentMsgID = 0; 
					m_CurrentMessage.data = nullptr;
				}	
				else	
					m_IncompleteMessages.erase (msgID);
				break;
			}
		}
	}

	bool TunnelEndpoint::ConcatNextOutOfSequenceFragment (uint32_t msgID, TunnelMessageBlockEx& msg)
	{
		auto it = m_OutOfSequenceFragments.find ((uint64_t)msgID << 32 | msg.nextFragmentNum);
		if (it != m_OutOfSequenceFragments.end ())
		{
			LogPrint (eLogDebug, "TunnelMessage: Out-of-sequence fragment ", (int)msg.nextFragmentNum, " of message ", msgID, " found");
			size_t size = it->second.data->GetLength ();
			if (msg.data->len + size > msg.data->maxLen)
			{
				LogPrint (eLogWarning, "TunnelMessage: Tunnel endpoint I2NP message size ", msg.data->maxLen, " is not enough");
				auto newMsg = NewI2NPMessage ();
				*newMsg = *(msg.data);
				msg.data = newMsg;
			}
			if (msg.data->Concat (it->second.data->GetBuffer (), size) < size) // concatenate out-of-sync fragment
				LogPrint (eLogError, "TunnelMessage: Tunnel endpoint I2NP buffer overflow ", msg.data->maxLen);
			if (it->second.isLastFragment)
				// message complete
				msg.nextFragmentNum = 0;
			else
				msg.nextFragmentNum++;
			m_OutOfSequenceFragments.erase (it);
			return true;
		}
		return false;
	}

	void TunnelEndpoint::HandleNextMessage (const TunnelMessageBlock& msg)
	{
		if (!m_IsInbound && msg.data->IsExpired ())
		{
			LogPrint (eLogInfo, "TunnelMessage: message expired");
			return;
		}
		uint8_t typeID = msg.data->GetTypeID ();
		LogPrint (eLogDebug, "TunnelMessage: handle fragment of ", msg.data->GetLength (), " bytes, msg type ", (int)typeID);
		// catch RI or reply with new list of routers
		if ((IsRouterInfoMsg (msg.data) || typeID == eI2NPDatabaseSearchReply) &&
			!m_IsInbound && msg.deliveryType != eDeliveryTypeLocal)
			i2p::data::netdb.PostI2NPMsg (CopyI2NPMessage (msg.data));

		switch (msg.deliveryType)
		{
			case eDeliveryTypeLocal:
				i2p::HandleI2NPMessage (msg.data);
			break;
			case eDeliveryTypeTunnel:
				if (!m_IsInbound) // outbound transit tunnel
					i2p::transport::transports.SendMessage (msg.hash, i2p::CreateTunnelGatewayMsg (msg.tunnelID, msg.data));
				else
					LogPrint (eLogError, "TunnelMessage: Delivery type 'tunnel' arrived from an inbound tunnel, dropped");
			break;
			case eDeliveryTypeRouter:
				if (!m_IsInbound) // outbound transit tunnel
					i2p::transport::transports.SendMessage (msg.hash, msg.data);
				else // we shouldn't send this message. possible leakage
					LogPrint (eLogError, "TunnelMessage: Delivery type 'router' arrived from an inbound tunnel, dropped");
			break;
			default:
				LogPrint (eLogError, "TunnelMessage: Unknown delivery type ", (int)msg.deliveryType);
		};
	}

	void TunnelEndpoint::Cleanup ()
	{
		auto ts = i2p::util::GetMillisecondsSinceEpoch ();
		// out-of-sequence fragments
		for (auto it = m_OutOfSequenceFragments.begin (); it != m_OutOfSequenceFragments.end ();)
		{
			if (ts > it->second.receiveTime + i2p::I2NP_MESSAGE_EXPIRATION_TIMEOUT)
				it = m_OutOfSequenceFragments.erase (it);
			else
				++it;
		}
		// incomplete messages
		for (auto it = m_IncompleteMessages.begin (); it != m_IncompleteMessages.end ();)
		{
			if (ts > it->second.receiveTime + i2p::I2NP_MESSAGE_EXPIRATION_TIMEOUT)
				it = m_IncompleteMessages.erase (it);
			else
				++it;
		}
	}
}
}
