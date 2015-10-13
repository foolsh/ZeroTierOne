/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2015  ZeroTier, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../version.h"
#include "../include/ZeroTierOne.h"

#include "Constants.hpp"
#include "RuntimeEnvironment.hpp"
#include "IncomingPacket.hpp"
#include "Topology.hpp"
#include "Switch.hpp"
#include "Peer.hpp"
#include "NetworkController.hpp"
#include "SelfAwareness.hpp"
#include "Salsa20.hpp"
#include "SHA512.hpp"
#include "World.hpp"

namespace ZeroTier {

bool IncomingPacket::tryDecode(const RuntimeEnvironment *RR)
{
	const Address sourceAddress(source());
	try {
		if ((cipher() == ZT_PROTO_CIPHER_SUITE__C25519_POLY1305_NONE)&&(verb() == Packet::VERB_HELLO)) {
			// Unencrypted HELLOs are handled here since they are used to
			// populate our identity cache in the first place. _doHELLO() is special
			// in that it contains its own authentication logic.
			return _doHELLO(RR);
		}

		SharedPtr<Peer> peer = RR->topology->getPeer(sourceAddress);
		if (peer) {
			if (!dearmor(peer->key())) {
				TRACE("dropped packet from %s(%s), MAC authentication failed (size: %u)",peer->address().toString().c_str(),_remoteAddress.toString().c_str(),size());
				return true;
			}
			if (!uncompress()) {
				TRACE("dropped packet from %s(%s), compressed data invalid",peer->address().toString().c_str(),_remoteAddress.toString().c_str());
				return true;
			}

			//TRACE("<< %s from %s(%s)",Packet::verbString(v),sourceAddress.toString().c_str(),_remoteAddress.toString().c_str());

			const Packet::Verb v = verb();
			switch(v) {
				//case Packet::VERB_NOP:
				default: // ignore unknown verbs, but if they pass auth check they are "received"
					peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),v,0,Packet::VERB_NOP);
					return true;
				case Packet::VERB_HELLO:                          return _doHELLO(RR);
				case Packet::VERB_ERROR:                          return _doERROR(RR,peer);
				case Packet::VERB_OK:                             return _doOK(RR,peer);
				case Packet::VERB_WHOIS:                          return _doWHOIS(RR,peer);
				case Packet::VERB_RENDEZVOUS:                     return _doRENDEZVOUS(RR,peer);
				case Packet::VERB_FRAME:                          return _doFRAME(RR,peer);
				case Packet::VERB_EXT_FRAME:                      return _doEXT_FRAME(RR,peer);
				case Packet::VERB_ECHO:                           return _doECHO(RR,peer);
				case Packet::VERB_MULTICAST_LIKE:                 return _doMULTICAST_LIKE(RR,peer);
				case Packet::VERB_NETWORK_MEMBERSHIP_CERTIFICATE: return _doNETWORK_MEMBERSHIP_CERTIFICATE(RR,peer);
				case Packet::VERB_NETWORK_CONFIG_REQUEST:         return _doNETWORK_CONFIG_REQUEST(RR,peer);
				case Packet::VERB_NETWORK_CONFIG_REFRESH:         return _doNETWORK_CONFIG_REFRESH(RR,peer);
				case Packet::VERB_MULTICAST_GATHER:               return _doMULTICAST_GATHER(RR,peer);
				case Packet::VERB_MULTICAST_FRAME:                return _doMULTICAST_FRAME(RR,peer);
				case Packet::VERB_PUSH_DIRECT_PATHS:              return _doPUSH_DIRECT_PATHS(RR,peer);
				case Packet::VERB_CIRCUIT_TEST:                   return _doCIRCUIT_TEST(RR,peer);
				case Packet::VERB_CIRCUIT_TEST_REPORT:            return _doCIRCUIT_TEST_REPORT(RR,peer);
				case Packet::VERB_REQUEST_PROOF_OF_WORK:          return _doREQUEST_PROOF_OF_WORK(RR,peer);
			}
		} else {
			RR->sw->requestWhois(sourceAddress);
			return false;
		}
	} catch ( ... ) {
		// Exceptions are more informatively caught in _do...() handlers but
		// this outer try/catch will catch anything else odd.
		TRACE("dropped ??? from %s(%s): unexpected exception in tryDecode()",sourceAddress.toString().c_str(),_remoteAddress.toString().c_str());
		return true;
	}
}

bool IncomingPacket::_doERROR(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		const Packet::Verb inReVerb = (Packet::Verb)(*this)[ZT_PROTO_VERB_ERROR_IDX_IN_RE_VERB];
		const uint64_t inRePacketId = at<uint64_t>(ZT_PROTO_VERB_ERROR_IDX_IN_RE_PACKET_ID);
		const Packet::ErrorCode errorCode = (Packet::ErrorCode)(*this)[ZT_PROTO_VERB_ERROR_IDX_ERROR_CODE];

		//TRACE("ERROR %s from %s(%s) in-re %s",Packet::errorString(errorCode),peer->address().toString().c_str(),_remoteAddress.toString().c_str(),Packet::verbString(inReVerb));

		switch(errorCode) {

			case Packet::ERROR_OBJ_NOT_FOUND:
				if (inReVerb == Packet::VERB_WHOIS) {
					if (RR->topology->isRoot(peer->identity()))
						RR->sw->cancelWhoisRequest(Address(field(ZT_PROTO_VERB_ERROR_IDX_PAYLOAD,ZT_ADDRESS_LENGTH),ZT_ADDRESS_LENGTH));
				} else if (inReVerb == Packet::VERB_NETWORK_CONFIG_REQUEST) {
					SharedPtr<Network> network(RR->node->network(at<uint64_t>(ZT_PROTO_VERB_ERROR_IDX_PAYLOAD)));
					if ((network)&&(network->controller() == peer->address()))
						network->setNotFound();
				}
				break;

			case Packet::ERROR_UNSUPPORTED_OPERATION:
				if (inReVerb == Packet::VERB_NETWORK_CONFIG_REQUEST) {
					SharedPtr<Network> network(RR->node->network(at<uint64_t>(ZT_PROTO_VERB_ERROR_IDX_PAYLOAD)));
					if ((network)&&(network->controller() == peer->address()))
						network->setNotFound();
				}
				break;

			case Packet::ERROR_IDENTITY_COLLISION:
				if (RR->topology->isRoot(peer->identity()))
					RR->node->postEvent(ZT_EVENT_FATAL_ERROR_IDENTITY_COLLISION);
				break;

			case Packet::ERROR_NEED_MEMBERSHIP_CERTIFICATE: {
				/* Note: certificates are public so it's safe to push them to anyone
				 * who asks. We won't communicate unless we also get a certificate
				 * from the remote that agrees. */
				SharedPtr<Network> network(RR->node->network(at<uint64_t>(ZT_PROTO_VERB_ERROR_IDX_PAYLOAD)));
				if (network) {
					SharedPtr<NetworkConfig> nconf(network->config2());
					if (nconf) {
						Packet outp(peer->address(),RR->identity.address(),Packet::VERB_NETWORK_MEMBERSHIP_CERTIFICATE);
						nconf->com().serialize(outp);
						outp.armor(peer->key(),true);
						RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
					}
				}
			}	break;

			case Packet::ERROR_NETWORK_ACCESS_DENIED_: {
				SharedPtr<Network> network(RR->node->network(at<uint64_t>(ZT_PROTO_VERB_ERROR_IDX_PAYLOAD)));
				if ((network)&&(network->controller() == peer->address()))
					network->setAccessDenied();
			}	break;

			case Packet::ERROR_UNWANTED_MULTICAST: {
				uint64_t nwid = at<uint64_t>(ZT_PROTO_VERB_ERROR_IDX_PAYLOAD);
				MulticastGroup mg(MAC(field(ZT_PROTO_VERB_ERROR_IDX_PAYLOAD + 8,6),6),at<uint32_t>(ZT_PROTO_VERB_ERROR_IDX_PAYLOAD + 14));
				TRACE("%.16llx: peer %s unsubscrubed from multicast group %s",nwid,peer->address().toString().c_str(),mg.toString().c_str());
				RR->mc->remove(nwid,mg,peer->address());
			}	break;

			default: break;
		}

		peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_ERROR,inRePacketId,inReVerb);
	} catch ( ... ) {
		TRACE("dropped ERROR from %s(%s): unexpected exception",peer->address().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doHELLO(const RuntimeEnvironment *RR)
{
	/* Note: this is the only packet ever sent in the clear, and it's also
	 * the only packet that we authenticate via a different path. Authentication
	 * occurs here and is based on the validity of the identity and the
	 * integrity of the packet's MAC, but it must be done after we check
	 * the identity since HELLO is a mechanism for learning new identities
	 * in the first place. */

	try {
		const uint64_t pid = packetId();
		const Address fromAddress(source());
		const unsigned int protoVersion = (*this)[ZT_PROTO_VERB_HELLO_IDX_PROTOCOL_VERSION];
		const unsigned int vMajor = (*this)[ZT_PROTO_VERB_HELLO_IDX_MAJOR_VERSION];
		const unsigned int vMinor = (*this)[ZT_PROTO_VERB_HELLO_IDX_MINOR_VERSION];
		const unsigned int vRevision = at<uint16_t>(ZT_PROTO_VERB_HELLO_IDX_REVISION);
		const uint64_t timestamp = at<uint64_t>(ZT_PROTO_VERB_HELLO_IDX_TIMESTAMP);

		Identity id;
		InetAddress destAddr;
		uint64_t worldId = ZT_WORLD_ID_NULL;
		uint64_t worldTimestamp = 0;
		{
			unsigned int ptr = ZT_PROTO_VERB_HELLO_IDX_IDENTITY + id.deserialize(*this,ZT_PROTO_VERB_HELLO_IDX_IDENTITY);
			if (ptr < size()) // ZeroTier One < 1.0.3 did not include physical destination address info
				ptr += destAddr.deserialize(*this,ptr);
			if ((ptr + 16) <= size()) { // older versions also did not include World IDs or timestamps
				worldId = at<uint64_t>(ptr); ptr += 8;
				worldTimestamp = at<uint64_t>(ptr);
			}
		}

		if (protoVersion < ZT_PROTO_VERSION_MIN) {
			TRACE("dropped HELLO from %s(%s): protocol version too old",id.address().toString().c_str(),_remoteAddress.toString().c_str());
			return true;
		}
		if (fromAddress != id.address()) {
			TRACE("dropped HELLO from %s(%s): identity not for sending address",fromAddress.toString().c_str(),_remoteAddress.toString().c_str());
			return true;
		}

		SharedPtr<Peer> peer(RR->topology->getPeer(id.address()));
		if (peer) {
			// We already have an identity with this address -- check for collisions

			if (peer->identity() != id) {
				// Identity is different from the one we already have -- address collision

				unsigned char key[ZT_PEER_SECRET_KEY_LENGTH];
				if (RR->identity.agree(id,key,ZT_PEER_SECRET_KEY_LENGTH)) {
					if (dearmor(key)) { // ensure packet is authentic, otherwise drop
						TRACE("rejected HELLO from %s(%s): address already claimed",id.address().toString().c_str(),_remoteAddress.toString().c_str());
						Packet outp(id.address(),RR->identity.address(),Packet::VERB_ERROR);
						outp.append((unsigned char)Packet::VERB_HELLO);
						outp.append((uint64_t)pid);
						outp.append((unsigned char)Packet::ERROR_IDENTITY_COLLISION);
						outp.armor(key,true);
						RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
					} else {
						TRACE("rejected HELLO from %s(%s): packet failed authentication",id.address().toString().c_str(),_remoteAddress.toString().c_str());
					}
				} else {
					TRACE("rejected HELLO from %s(%s): key agreement failed",id.address().toString().c_str(),_remoteAddress.toString().c_str());
				}

				return true;
			} else {
				// Identity is the same as the one we already have -- check packet integrity

				if (!dearmor(peer->key())) {
					TRACE("rejected HELLO from %s(%s): packet failed authentication",id.address().toString().c_str(),_remoteAddress.toString().c_str());
					return true;
				}

				// Continue at // VALID
			}
		} else {
			// We don't already have an identity with this address -- validate and learn it

			// Check identity proof of work
			if (!id.locallyValidate()) {
				TRACE("dropped HELLO from %s(%s): identity invalid",id.address().toString().c_str(),_remoteAddress.toString().c_str());
				return true;
			}

			// Check packet integrity and authentication
			SharedPtr<Peer> newPeer(new Peer(RR->identity,id));
			if (!dearmor(newPeer->key())) {
				TRACE("rejected HELLO from %s(%s): packet failed authentication",id.address().toString().c_str(),_remoteAddress.toString().c_str());
				return true;
			}

			peer = RR->topology->addPeer(newPeer);

			// Continue at // VALID
		}

		// VALID -- if we made it here, packet passed identity and authenticity checks!

		peer->received(RR,_localAddress,_remoteAddress,hops(),pid,Packet::VERB_HELLO,0,Packet::VERB_NOP);
		peer->setRemoteVersion(protoVersion,vMajor,vMinor,vRevision);

		if (destAddr)
			RR->sa->iam(id.address(),_remoteAddress,destAddr,RR->topology->isRoot(id),RR->node->now());

		Packet outp(id.address(),RR->identity.address(),Packet::VERB_OK);
		outp.append((unsigned char)Packet::VERB_HELLO);
		outp.append((uint64_t)pid);
		outp.append((uint64_t)timestamp);
		outp.append((unsigned char)ZT_PROTO_VERSION);
		outp.append((unsigned char)ZEROTIER_ONE_VERSION_MAJOR);
		outp.append((unsigned char)ZEROTIER_ONE_VERSION_MINOR);
		outp.append((uint16_t)ZEROTIER_ONE_VERSION_REVISION);
		_remoteAddress.serialize(outp);

		if ((worldId != ZT_WORLD_ID_NULL)&&(worldId == RR->topology->worldId())) {
			if (RR->topology->worldTimestamp() > worldTimestamp) {
				World w(RR->topology->world());
				const unsigned int sizeAt = outp.size();
				outp.addSize(2); // make room for 16-bit size field
				w.serialize(outp,false);
				outp.setAt<uint16_t>(sizeAt,(uint16_t)(outp.size() - sizeAt));
			} else {
				outp.append((uint16_t)0); // no world update needed
			}

			outp.armor(peer->key(),true);
			RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
		} else {
			TRACE("dropped HELLO from %s(%s): world ID mismatch: peer is %llu and we are %llu",source().toString().c_str(),_remoteAddress.toString().c_str(),worldId,RR->topology->worldId());
		}
	} catch ( ... ) {
		TRACE("dropped HELLO from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doOK(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		const Packet::Verb inReVerb = (Packet::Verb)(*this)[ZT_PROTO_VERB_OK_IDX_IN_RE_VERB];
		const uint64_t inRePacketId = at<uint64_t>(ZT_PROTO_VERB_OK_IDX_IN_RE_PACKET_ID);

		//TRACE("%s(%s): OK(%s)",source().toString().c_str(),_remoteAddress.toString().c_str(),Packet::verbString(inReVerb));

		switch(inReVerb) {

			case Packet::VERB_HELLO: {
				const unsigned int latency = std::min((unsigned int)(RR->node->now() - at<uint64_t>(ZT_PROTO_VERB_HELLO__OK__IDX_TIMESTAMP)),(unsigned int)0xffff);
				const unsigned int vProto = (*this)[ZT_PROTO_VERB_HELLO__OK__IDX_PROTOCOL_VERSION];
				const unsigned int vMajor = (*this)[ZT_PROTO_VERB_HELLO__OK__IDX_MAJOR_VERSION];
				const unsigned int vMinor = (*this)[ZT_PROTO_VERB_HELLO__OK__IDX_MINOR_VERSION];
				const unsigned int vRevision = at<uint16_t>(ZT_PROTO_VERB_HELLO__OK__IDX_REVISION);

				InetAddress destAddr;
				if ((ZT_PROTO_VERB_HELLO__OK__IDX_REVISION + 2) < size()) // ZeroTier One < 1.0.3 did not include this field
					destAddr.deserialize(*this,ZT_PROTO_VERB_HELLO__OK__IDX_REVISION + 2);

				if (vProto < ZT_PROTO_VERSION_MIN) {
					TRACE("%s(%s): OK(HELLO) dropped, protocol version too old",source().toString().c_str(),_remoteAddress.toString().c_str());
					return true;
				}

				TRACE("%s(%s): OK(HELLO), version %u.%u.%u, latency %u, reported external address %s",source().toString().c_str(),_remoteAddress.toString().c_str(),vMajor,vMinor,vRevision,latency,((destAddr) ? destAddr.toString().c_str() : "(none)"));

				peer->addDirectLatencyMeasurment(latency);
				peer->setRemoteVersion(vProto,vMajor,vMinor,vRevision);

				bool trusted = RR->topology->isRoot(peer->identity());
				if (destAddr)
					RR->sa->iam(peer->address(),_remoteAddress,destAddr,trusted,RR->node->now());
			}	break;

			case Packet::VERB_WHOIS: {
				/* Right now only root servers are allowed to send OK(WHOIS) to prevent
				 * poisoning attacks. Further decentralization will require some other
				 * kind of trust mechanism. */
				if (RR->topology->isRoot(peer->identity())) {
					const Identity id(*this,ZT_PROTO_VERB_WHOIS__OK__IDX_IDENTITY);
					if (id.locallyValidate())
						RR->sw->doAnythingWaitingForPeer(RR->topology->addPeer(SharedPtr<Peer>(new Peer(RR->identity,id))));
				}
			} break;

			case Packet::VERB_NETWORK_CONFIG_REQUEST: {
				const SharedPtr<Network> nw(RR->node->network(at<uint64_t>(ZT_PROTO_VERB_NETWORK_CONFIG_REQUEST__OK__IDX_NETWORK_ID)));
				if ((nw)&&(nw->controller() == peer->address())) {
					const unsigned int dictlen = at<uint16_t>(ZT_PROTO_VERB_NETWORK_CONFIG_REQUEST__OK__IDX_DICT_LEN);
					const std::string dict((const char *)field(ZT_PROTO_VERB_NETWORK_CONFIG_REQUEST__OK__IDX_DICT,dictlen),dictlen);
					if (dict.length()) {
						nw->setConfiguration(Dictionary(dict));
						TRACE("got network configuration for network %.16llx from %s",(unsigned long long)nw->id(),source().toString().c_str());
					}
				}
			}	break;

			case Packet::VERB_MULTICAST_GATHER: {
				const uint64_t nwid = at<uint64_t>(ZT_PROTO_VERB_MULTICAST_GATHER__OK__IDX_NETWORK_ID);
				const MulticastGroup mg(MAC(field(ZT_PROTO_VERB_MULTICAST_GATHER__OK__IDX_MAC,6),6),at<uint32_t>(ZT_PROTO_VERB_MULTICAST_GATHER__OK__IDX_ADI));
				TRACE("%s(%s): OK(MULTICAST_GATHER) %.16llx/%s length %u",source().toString().c_str(),_remoteAddress.toString().c_str(),nwid,mg.toString().c_str(),size());
				const unsigned int count = at<uint16_t>(ZT_PROTO_VERB_MULTICAST_GATHER__OK__IDX_GATHER_RESULTS + 4);
				RR->mc->addMultiple(RR->node->now(),nwid,mg,field(ZT_PROTO_VERB_MULTICAST_GATHER__OK__IDX_GATHER_RESULTS + 6,count * 5),count,at<uint32_t>(ZT_PROTO_VERB_MULTICAST_GATHER__OK__IDX_GATHER_RESULTS));
			}	break;

			case Packet::VERB_MULTICAST_FRAME: {
				const unsigned int flags = (*this)[ZT_PROTO_VERB_MULTICAST_FRAME__OK__IDX_FLAGS];
				const uint64_t nwid = at<uint64_t>(ZT_PROTO_VERB_MULTICAST_FRAME__OK__IDX_NETWORK_ID);
				const MulticastGroup mg(MAC(field(ZT_PROTO_VERB_MULTICAST_FRAME__OK__IDX_MAC,6),6),at<uint32_t>(ZT_PROTO_VERB_MULTICAST_FRAME__OK__IDX_ADI));

				//TRACE("%s(%s): OK(MULTICAST_FRAME) %.16llx/%s flags %.2x",peer->address().toString().c_str(),_remoteAddress.toString().c_str(),nwid,mg.toString().c_str(),flags);

				unsigned int offset = 0;

				if ((flags & 0x01) != 0) {
					// OK(MULTICAST_FRAME) includes certificate of membership update
					CertificateOfMembership com;
					offset += com.deserialize(*this,ZT_PROTO_VERB_MULTICAST_FRAME__OK__IDX_COM_AND_GATHER_RESULTS);
					peer->validateAndSetNetworkMembershipCertificate(RR,nwid,com);
				}

				if ((flags & 0x02) != 0) {
					// OK(MULTICAST_FRAME) includes implicit gather results
					offset += ZT_PROTO_VERB_MULTICAST_FRAME__OK__IDX_COM_AND_GATHER_RESULTS;
					unsigned int totalKnown = at<uint32_t>(offset); offset += 4;
					unsigned int count = at<uint16_t>(offset); offset += 2;
					RR->mc->addMultiple(RR->node->now(),nwid,mg,field(offset,count * 5),count,totalKnown);
				}
			}	break;

			default: break;
		}

		peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_OK,inRePacketId,inReVerb);
	} catch ( ... ) {
		TRACE("dropped OK from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doWHOIS(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		if (payloadLength() == ZT_ADDRESS_LENGTH) {
			const SharedPtr<Peer> queried(RR->topology->getPeer(Address(payload(),ZT_ADDRESS_LENGTH)));
			if (queried) {
				Packet outp(peer->address(),RR->identity.address(),Packet::VERB_OK);
				outp.append((unsigned char)Packet::VERB_WHOIS);
				outp.append(packetId());
				queried->identity().serialize(outp,false);
				outp.armor(peer->key(),true);
				RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
			} else {
				Packet outp(peer->address(),RR->identity.address(),Packet::VERB_ERROR);
				outp.append((unsigned char)Packet::VERB_WHOIS);
				outp.append(packetId());
				outp.append((unsigned char)Packet::ERROR_OBJ_NOT_FOUND);
				outp.append(payload(),ZT_ADDRESS_LENGTH);
				outp.armor(peer->key(),true);
				RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
			}
		} else {
			TRACE("dropped WHOIS from %s(%s): missing or invalid address",source().toString().c_str(),_remoteAddress.toString().c_str());
		}
		peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_WHOIS,0,Packet::VERB_NOP);
	} catch ( ... ) {
		TRACE("dropped WHOIS from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doRENDEZVOUS(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		const Address with(field(ZT_PROTO_VERB_RENDEZVOUS_IDX_ZTADDRESS,ZT_ADDRESS_LENGTH),ZT_ADDRESS_LENGTH);
		const SharedPtr<Peer> withPeer(RR->topology->getPeer(with));
		if (withPeer) {
			const unsigned int port = at<uint16_t>(ZT_PROTO_VERB_RENDEZVOUS_IDX_PORT);
			const unsigned int addrlen = (*this)[ZT_PROTO_VERB_RENDEZVOUS_IDX_ADDRLEN];
			if ((port > 0)&&((addrlen == 4)||(addrlen == 16))) {
				InetAddress atAddr(field(ZT_PROTO_VERB_RENDEZVOUS_IDX_ADDRESS,addrlen),addrlen,port);
				TRACE("RENDEZVOUS from %s says %s might be at %s, starting NAT-t",peer->address().toString().c_str(),with.toString().c_str(),atAddr.toString().c_str());
				peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_RENDEZVOUS,0,Packet::VERB_NOP);
				RR->sw->rendezvous(withPeer,_localAddress,atAddr);
			} else {
				TRACE("dropped corrupt RENDEZVOUS from %s(%s) (bad address or port)",peer->address().toString().c_str(),_remoteAddress.toString().c_str());
			}
		} else {
			TRACE("ignored RENDEZVOUS from %s(%s) to meet unknown peer %s",peer->address().toString().c_str(),_remoteAddress.toString().c_str(),with.toString().c_str());
		}
	} catch ( ... ) {
		TRACE("dropped RENDEZVOUS from %s(%s): unexpected exception",peer->address().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doFRAME(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		const SharedPtr<Network> network(RR->node->network(at<uint64_t>(ZT_PROTO_VERB_FRAME_IDX_NETWORK_ID)));
		if (network) {
			if (size() > ZT_PROTO_VERB_FRAME_IDX_PAYLOAD) {
				if (!network->isAllowed(peer)) {
					TRACE("dropped FRAME from %s(%s): not a member of private network %.16llx",peer->address().toString().c_str(),_remoteAddress.toString().c_str(),(unsigned long long)network->id());
					_sendErrorNeedCertificate(RR,peer,network->id());
					return true;
				}

				const unsigned int etherType = at<uint16_t>(ZT_PROTO_VERB_FRAME_IDX_ETHERTYPE);
				if (!network->config()->permitsEtherType(etherType)) {
					TRACE("dropped FRAME from %s(%s): ethertype %.4x not allowed on %.16llx",peer->address().toString().c_str(),_remoteAddress.toString().c_str(),(unsigned int)etherType,(unsigned long long)network->id());
					return true;
				}

				const unsigned int payloadLen = size() - ZT_PROTO_VERB_FRAME_IDX_PAYLOAD;
				RR->node->putFrame(network->id(),MAC(peer->address(),network->id()),network->mac(),etherType,0,field(ZT_PROTO_VERB_FRAME_IDX_PAYLOAD,payloadLen),payloadLen);
			}

			peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_FRAME,0,Packet::VERB_NOP);
		} else {
			TRACE("dropped FRAME from %s(%s): we are not connected to network %.16llx",source().toString().c_str(),_remoteAddress.toString().c_str(),at<uint64_t>(ZT_PROTO_VERB_FRAME_IDX_NETWORK_ID));
		}
	} catch ( ... ) {
		TRACE("dropped FRAME from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doEXT_FRAME(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		SharedPtr<Network> network(RR->node->network(at<uint64_t>(ZT_PROTO_VERB_EXT_FRAME_IDX_NETWORK_ID)));
		if (network) {
			if (size() > ZT_PROTO_VERB_EXT_FRAME_IDX_PAYLOAD) {
				const unsigned int flags = (*this)[ZT_PROTO_VERB_EXT_FRAME_IDX_FLAGS];

				unsigned int comLen = 0;
				bool comFailed = false;
				if ((flags & 0x01) != 0) {
					CertificateOfMembership com;
					comLen = com.deserialize(*this,ZT_PROTO_VERB_EXT_FRAME_IDX_COM);
					if (!peer->validateAndSetNetworkMembershipCertificate(RR,network->id(),com))
						comFailed = true;
				}

				if ((comFailed)||(!network->isAllowed(peer))) {
					TRACE("dropped EXT_FRAME from %s(%s): not a member of private network %.16llx",peer->address().toString().c_str(),_remoteAddress.toString().c_str(),network->id());
					_sendErrorNeedCertificate(RR,peer,network->id());
					return true;
				}

				// Everything after flags must be adjusted based on the length
				// of the certificate, if there was one...

				const unsigned int etherType = at<uint16_t>(comLen + ZT_PROTO_VERB_EXT_FRAME_IDX_ETHERTYPE);
				if (!network->config()->permitsEtherType(etherType)) {
					TRACE("dropped EXT_FRAME from %s(%s): ethertype %.4x not allowed on network %.16llx",peer->address().toString().c_str(),_remoteAddress.toString().c_str(),(unsigned int)etherType,(unsigned long long)network->id());
					return true;
				}

				const MAC to(field(comLen + ZT_PROTO_VERB_EXT_FRAME_IDX_TO,ZT_PROTO_VERB_EXT_FRAME_LEN_TO),ZT_PROTO_VERB_EXT_FRAME_LEN_TO);
				const MAC from(field(comLen + ZT_PROTO_VERB_EXT_FRAME_IDX_FROM,ZT_PROTO_VERB_EXT_FRAME_LEN_FROM),ZT_PROTO_VERB_EXT_FRAME_LEN_FROM);

				if (to.isMulticast()) {
					TRACE("dropped EXT_FRAME from %s@%s(%s) to %s: destination is multicast, must use MULTICAST_FRAME",from.toString().c_str(),peer->address().toString().c_str(),_remoteAddress.toString().c_str(),to.toString().c_str());
					return true;
				}

				if ((!from)||(from.isMulticast())||(from == network->mac())) {
					TRACE("dropped EXT_FRAME from %s@%s(%s) to %s: invalid source MAC",from.toString().c_str(),peer->address().toString().c_str(),_remoteAddress.toString().c_str(),to.toString().c_str());
					return true;
				}

				if (from != MAC(peer->address(),network->id())) {
					if (network->permitsBridging(peer->address())) {
						network->learnBridgeRoute(from,peer->address());
					} else {
						TRACE("dropped EXT_FRAME from %s@%s(%s) to %s: sender not allowed to bridge into %.16llx",from.toString().c_str(),peer->address().toString().c_str(),_remoteAddress.toString().c_str(),to.toString().c_str(),network->id());
						return true;
					}
				} else if (to != network->mac()) {
					if (!network->permitsBridging(RR->identity.address())) {
						TRACE("dropped EXT_FRAME from %s@%s(%s) to %s: I cannot bridge to %.16llx or bridging disabled on network",from.toString().c_str(),peer->address().toString().c_str(),_remoteAddress.toString().c_str(),to.toString().c_str(),network->id());
						return true;
					}
				}

				const unsigned int payloadLen = size() - (comLen + ZT_PROTO_VERB_EXT_FRAME_IDX_PAYLOAD);
				RR->node->putFrame(network->id(),from,to,etherType,0,field(comLen + ZT_PROTO_VERB_EXT_FRAME_IDX_PAYLOAD,payloadLen),payloadLen);
			}

			peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_EXT_FRAME,0,Packet::VERB_NOP);
		} else {
			TRACE("dropped EXT_FRAME from %s(%s): we are not connected to network %.16llx",source().toString().c_str(),_remoteAddress.toString().c_str(),at<uint64_t>(ZT_PROTO_VERB_FRAME_IDX_NETWORK_ID));
		}
	} catch ( ... ) {
		TRACE("dropped EXT_FRAME from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doECHO(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		const uint64_t pid = packetId();
		Packet outp(peer->address(),RR->identity.address(),Packet::VERB_OK);
		outp.append((unsigned char)Packet::VERB_ECHO);
		outp.append((uint64_t)pid);
		outp.append(field(ZT_PACKET_IDX_PAYLOAD,size() - ZT_PACKET_IDX_PAYLOAD),size() - ZT_PACKET_IDX_PAYLOAD);
		RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
		peer->received(RR,_localAddress,_remoteAddress,hops(),pid,Packet::VERB_ECHO,0,Packet::VERB_NOP);
	} catch ( ... ) {
		TRACE("dropped ECHO from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doMULTICAST_LIKE(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		const uint64_t now = RR->node->now();

		// Iterate through 18-byte network,MAC,ADI tuples
		for(unsigned int ptr=ZT_PACKET_IDX_PAYLOAD;ptr<size();ptr+=18)
			RR->mc->add(now,at<uint64_t>(ptr),MulticastGroup(MAC(field(ptr + 8,6),6),at<uint32_t>(ptr + 14)),peer->address());

		peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_MULTICAST_LIKE,0,Packet::VERB_NOP);
	} catch ( ... ) {
		TRACE("dropped MULTICAST_LIKE from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doNETWORK_MEMBERSHIP_CERTIFICATE(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		CertificateOfMembership com;

		unsigned int ptr = ZT_PACKET_IDX_PAYLOAD;
		while (ptr < size()) {
			ptr += com.deserialize(*this,ptr);
			peer->validateAndSetNetworkMembershipCertificate(RR,com.networkId(),com);
		}

		peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_NETWORK_MEMBERSHIP_CERTIFICATE,0,Packet::VERB_NOP);
	} catch ( ... ) {
		TRACE("dropped NETWORK_MEMBERSHIP_CERTIFICATE from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doNETWORK_CONFIG_REQUEST(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		const uint64_t nwid = at<uint64_t>(ZT_PROTO_VERB_NETWORK_CONFIG_REQUEST_IDX_NETWORK_ID);
		const unsigned int metaDataLength = at<uint16_t>(ZT_PROTO_VERB_NETWORK_CONFIG_REQUEST_IDX_DICT_LEN);
		const Dictionary metaData((const char *)field(ZT_PROTO_VERB_NETWORK_CONFIG_REQUEST_IDX_DICT,metaDataLength),metaDataLength);
		//const uint64_t haveRevision = ((ZT_PROTO_VERB_NETWORK_CONFIG_REQUEST_IDX_DICT + metaDataLength + 8) <= size()) ? at<uint64_t>(ZT_PROTO_VERB_NETWORK_CONFIG_REQUEST_IDX_DICT + metaDataLength) : 0ULL;

		const unsigned int h = hops();
		const uint64_t pid = packetId();
		peer->received(RR,_localAddress,_remoteAddress,h,pid,Packet::VERB_NETWORK_CONFIG_REQUEST,0,Packet::VERB_NOP);

		if (RR->localNetworkController) {
			Dictionary netconf;
			switch(RR->localNetworkController->doNetworkConfigRequest((h > 0) ? InetAddress() : _remoteAddress,RR->identity,peer->identity(),nwid,metaData,netconf)) {

				case NetworkController::NETCONF_QUERY_OK: {
					const std::string netconfStr(netconf.toString());
					if (netconfStr.length() > 0xffff) { // sanity check since field ix 16-bit
						TRACE("NETWORK_CONFIG_REQUEST failed: internal error: netconf size %u is too large",(unsigned int)netconfStr.length());
					} else {
						Packet outp(peer->address(),RR->identity.address(),Packet::VERB_OK);
						outp.append((unsigned char)Packet::VERB_NETWORK_CONFIG_REQUEST);
						outp.append(pid);
						outp.append(nwid);
						outp.append((uint16_t)netconfStr.length());
						outp.append(netconfStr.data(),(unsigned int)netconfStr.length());
						outp.compress();
						outp.armor(peer->key(),true);
						if (outp.size() > ZT_PROTO_MAX_PACKET_LENGTH) { // sanity check
							TRACE("NETWORK_CONFIG_REQUEST failed: internal error: netconf size %u is too large",(unsigned int)netconfStr.length());
						} else {
							RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
						}
					}
				}	break;

				case NetworkController::NETCONF_QUERY_OBJECT_NOT_FOUND: {
					Packet outp(peer->address(),RR->identity.address(),Packet::VERB_ERROR);
					outp.append((unsigned char)Packet::VERB_NETWORK_CONFIG_REQUEST);
					outp.append(pid);
					outp.append((unsigned char)Packet::ERROR_OBJ_NOT_FOUND);
					outp.append(nwid);
					outp.armor(peer->key(),true);
					RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
				}	break;

				case NetworkController::NETCONF_QUERY_ACCESS_DENIED: {
					Packet outp(peer->address(),RR->identity.address(),Packet::VERB_ERROR);
					outp.append((unsigned char)Packet::VERB_NETWORK_CONFIG_REQUEST);
					outp.append(pid);
					outp.append((unsigned char)Packet::ERROR_NETWORK_ACCESS_DENIED_);
					outp.append(nwid);
					outp.armor(peer->key(),true);
					RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
				} break;

				case NetworkController::NETCONF_QUERY_INTERNAL_SERVER_ERROR:
					TRACE("NETWORK_CONFIG_REQUEST failed: internal error: %s",netconf.get("error","(unknown)").c_str());
					break;

				case NetworkController::NETCONF_QUERY_IGNORE:
					break;

				default:
					TRACE("NETWORK_CONFIG_REQUEST failed: invalid return value from NetworkController::doNetworkConfigRequest()");
					break;

			}
		} else {
			Packet outp(peer->address(),RR->identity.address(),Packet::VERB_ERROR);
			outp.append((unsigned char)Packet::VERB_NETWORK_CONFIG_REQUEST);
			outp.append(pid);
			outp.append((unsigned char)Packet::ERROR_UNSUPPORTED_OPERATION);
			outp.append(nwid);
			outp.armor(peer->key(),true);
			RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
		}
	} catch ( ... ) {
		TRACE("dropped NETWORK_CONFIG_REQUEST from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doNETWORK_CONFIG_REFRESH(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		unsigned int ptr = ZT_PACKET_IDX_PAYLOAD;
		while ((ptr + 8) <= size()) {
			uint64_t nwid = at<uint64_t>(ptr);
			SharedPtr<Network> nw(RR->node->network(nwid));
			if ((nw)&&(peer->address() == nw->controller()))
				nw->requestConfiguration();
			ptr += 8;
		}
		peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_NETWORK_CONFIG_REFRESH,0,Packet::VERB_NOP);
	} catch ( ... ) {
		TRACE("dropped NETWORK_CONFIG_REFRESH from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doMULTICAST_GATHER(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		const uint64_t nwid = at<uint64_t>(ZT_PROTO_VERB_MULTICAST_GATHER_IDX_NETWORK_ID);
		const MulticastGroup mg(MAC(field(ZT_PROTO_VERB_MULTICAST_GATHER_IDX_MAC,6),6),at<uint32_t>(ZT_PROTO_VERB_MULTICAST_GATHER_IDX_ADI));
		const unsigned int gatherLimit = at<uint32_t>(ZT_PROTO_VERB_MULTICAST_GATHER_IDX_GATHER_LIMIT);

		//TRACE("<<MC %s(%s) GATHER up to %u in %.16llx/%s",source().toString().c_str(),_remoteAddress.toString().c_str(),gatherLimit,nwid,mg.toString().c_str());

		if (gatherLimit) {
			Packet outp(peer->address(),RR->identity.address(),Packet::VERB_OK);
			outp.append((unsigned char)Packet::VERB_MULTICAST_GATHER);
			outp.append(packetId());
			outp.append(nwid);
			mg.mac().appendTo(outp);
			outp.append((uint32_t)mg.adi());
			if (RR->mc->gather(peer->address(),nwid,mg,outp,gatherLimit)) {
				outp.armor(peer->key(),true);
				RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
			}
		}

		peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_MULTICAST_GATHER,0,Packet::VERB_NOP);
	} catch ( ... ) {
		TRACE("dropped MULTICAST_GATHER from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doMULTICAST_FRAME(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		const uint64_t nwid = at<uint64_t>(ZT_PROTO_VERB_MULTICAST_FRAME_IDX_NETWORK_ID);
		const unsigned int flags = (*this)[ZT_PROTO_VERB_MULTICAST_FRAME_IDX_FLAGS];

		const SharedPtr<Network> network(RR->node->network(nwid));
		if (network) {
			// Offset -- size of optional fields added to position of later fields
			unsigned int offset = 0;

			if ((flags & 0x01) != 0) {
				CertificateOfMembership com;
				offset += com.deserialize(*this,ZT_PROTO_VERB_MULTICAST_FRAME_IDX_COM);
				peer->validateAndSetNetworkMembershipCertificate(RR,nwid,com);
			}

			// Check membership after we've read any included COM, since
			// that cert might be what we needed.
			if (!network->isAllowed(peer)) {
				TRACE("dropped MULTICAST_FRAME from %s(%s): not a member of private network %.16llx",peer->address().toString().c_str(),_remoteAddress.toString().c_str(),(unsigned long long)network->id());
				_sendErrorNeedCertificate(RR,peer,network->id());
				return true;
			}

			unsigned int gatherLimit = 0;
			if ((flags & 0x02) != 0) {
				gatherLimit = at<uint32_t>(offset + ZT_PROTO_VERB_MULTICAST_FRAME_IDX_GATHER_LIMIT);
				offset += 4;
			}

			MAC from;
			if ((flags & 0x04) != 0) {
				from.setTo(field(offset + ZT_PROTO_VERB_MULTICAST_FRAME_IDX_SOURCE_MAC,6),6);
				offset += 6;
			} else {
				from.fromAddress(peer->address(),nwid);
			}

			const MulticastGroup to(MAC(field(offset + ZT_PROTO_VERB_MULTICAST_FRAME_IDX_DEST_MAC,6),6),at<uint32_t>(offset + ZT_PROTO_VERB_MULTICAST_FRAME_IDX_DEST_ADI));
			const unsigned int etherType = at<uint16_t>(offset + ZT_PROTO_VERB_MULTICAST_FRAME_IDX_ETHERTYPE);
			const unsigned int payloadLen = size() - (offset + ZT_PROTO_VERB_MULTICAST_FRAME_IDX_FRAME);

			//TRACE("<<MC FRAME %.16llx/%s from %s@%s flags %.2x length %u",nwid,to.toString().c_str(),from.toString().c_str(),peer->address().toString().c_str(),flags,payloadLen);

			if ((payloadLen > 0)&&(payloadLen <= ZT_IF_MTU)) {
				if (!to.mac().isMulticast()) {
					TRACE("dropped MULTICAST_FRAME from %s@%s(%s) to %s: destination is unicast, must use FRAME or EXT_FRAME",from.toString().c_str(),peer->address().toString().c_str(),_remoteAddress.toString().c_str(),to.toString().c_str());
					return true;
				}
				if ((!from)||(from.isMulticast())||(from == network->mac())) {
					TRACE("dropped MULTICAST_FRAME from %s@%s(%s) to %s: invalid source MAC",from.toString().c_str(),peer->address().toString().c_str(),_remoteAddress.toString().c_str(),to.toString().c_str());
					return true;
				}

				if (from != MAC(peer->address(),network->id())) {
					if (network->permitsBridging(peer->address())) {
						network->learnBridgeRoute(from,peer->address());
					} else {
						TRACE("dropped MULTICAST_FRAME from %s@%s(%s) to %s: sender not allowed to bridge into %.16llx",from.toString().c_str(),peer->address().toString().c_str(),_remoteAddress.toString().c_str(),to.toString().c_str(),network->id());
						return true;
					}
				}

				RR->node->putFrame(network->id(),from,to.mac(),etherType,0,field(offset + ZT_PROTO_VERB_MULTICAST_FRAME_IDX_FRAME,payloadLen),payloadLen);
			}

			if (gatherLimit) {
				Packet outp(source(),RR->identity.address(),Packet::VERB_OK);
				outp.append((unsigned char)Packet::VERB_MULTICAST_FRAME);
				outp.append(packetId());
				outp.append(nwid);
				to.mac().appendTo(outp);
				outp.append((uint32_t)to.adi());
				outp.append((unsigned char)0x02); // flag 0x02 = contains gather results
				if (RR->mc->gather(peer->address(),nwid,to,outp,gatherLimit)) {
					outp.armor(peer->key(),true);
					RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
				}
			}
		} // else ignore -- not a member of this network

		peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_MULTICAST_FRAME,0,Packet::VERB_NOP);
	} catch ( ... ) {
		TRACE("dropped MULTICAST_FRAME from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doPUSH_DIRECT_PATHS(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		unsigned int count = at<uint16_t>(ZT_PACKET_IDX_PAYLOAD);
		unsigned int ptr = ZT_PACKET_IDX_PAYLOAD + 2;

		while (count--) { // if ptr overflows Buffer will throw
			// TODO: some flags are not yet implemented

			unsigned int flags = (*this)[ptr++];
			unsigned int extLen = at<uint16_t>(ptr); ptr += 2;
			ptr += extLen; // unused right now
			unsigned int addrType = (*this)[ptr++];
			unsigned int addrLen = (*this)[ptr++];

			switch(addrType) {
				case 4: {
					InetAddress a(field(ptr,4),4,at<uint16_t>(ptr + 4));
					if ( ((flags & 0x01) == 0) && (Path::isAddressValidForPath(a)) ) {
						TRACE("attempting to contact %s at pushed direct path %s",peer->address().toString().c_str(),a.toString().c_str());
						peer->attemptToContactAt(RR,_localAddress,a,RR->node->now());
					}
				}	break;
				case 6: {
					InetAddress a(field(ptr,16),16,at<uint16_t>(ptr + 16));
					if ( ((flags & 0x01) == 0) && (Path::isAddressValidForPath(a)) ) {
						TRACE("attempting to contact %s at pushed direct path %s",peer->address().toString().c_str(),a.toString().c_str());
						peer->attemptToContactAt(RR,_localAddress,a,RR->node->now());
					}
				}	break;
			}
			ptr += addrLen;
		}

		peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_PUSH_DIRECT_PATHS,0,Packet::VERB_NOP);
	} catch ( ... ) {
		TRACE("dropped PUSH_DIRECT_PATHS from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doCIRCUIT_TEST(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		const Address originatorAddress(field(ZT_PACKET_IDX_PAYLOAD,ZT_ADDRESS_LENGTH),ZT_ADDRESS_LENGTH);
		SharedPtr<Peer> originator(RR->topology->getPeer(originatorAddress));
		if (!originator) {
			RR->sw->requestWhois(originatorAddress);
			return false;
		}

		const unsigned int flags = at<uint16_t>(ZT_PACKET_IDX_PAYLOAD + 5);
		const uint64_t timestamp = at<uint64_t>(ZT_PACKET_IDX_PAYLOAD + 7);
		const uint64_t testId = at<uint64_t>(ZT_PACKET_IDX_PAYLOAD + 15);

		// Tracks total length of variable length fields, initialized to originator credential length below
		unsigned int vlf;

		// Originator credentials
		const unsigned int originatorCredentialLength = vlf = at<uint16_t>(ZT_PACKET_IDX_PAYLOAD + 23);
		uint64_t originatorCredentialNetworkId = 0;
		if (originatorCredentialLength >= 1) {
			switch((*this)[ZT_PACKET_IDX_PAYLOAD + 25]) {
				case 0x01: { // 64-bit network ID, originator must be controller
					if (originatorCredentialLength >= 9)
						originatorCredentialNetworkId = at<uint64_t>(ZT_PACKET_IDX_PAYLOAD + 26);
				}	break;
				default: break;
			}
		}

		// Add length of "additional fields," which are currently unused
		vlf += at<uint16_t>(ZT_PACKET_IDX_PAYLOAD + 25 + vlf);

		// Verify signature -- only tests signed by their originators are allowed
		const unsigned int signatureLength = at<uint16_t>(ZT_PACKET_IDX_PAYLOAD + 27 + vlf);
		if (!originator->identity().verify(field(ZT_PACKET_IDX_PAYLOAD,27 + vlf),27 + vlf,field(ZT_PACKET_IDX_PAYLOAD + 29 + vlf,signatureLength),signatureLength)) {
			TRACE("dropped CIRCUIT_TEST from %s(%s): signature by originator %s invalid",source().toString().c_str(),_remoteAddress.toString().c_str(),originatorAddress.toString().c_str());
			return true;
		}
		vlf += signatureLength;

		// Save this length so we can copy the immutable parts of this test
		// into the one we send along to next hops.
		const unsigned int lengthOfSignedPortionAndSignature = 29 + vlf;

		// Get previous hop's credential, if any
		const unsigned int previousHopCredentialLength = at<uint16_t>(ZT_PACKET_IDX_PAYLOAD + 29 + vlf);
		CertificateOfMembership previousHopCom;
		if (previousHopCredentialLength >= 1) {
			switch((*this)[ZT_PACKET_IDX_PAYLOAD + 31 + vlf]) {
				case 0x01: { // network certificate of membership for previous hop
					if (previousHopCom.deserialize(*this,ZT_PACKET_IDX_PAYLOAD + 32 + vlf) != (previousHopCredentialLength - 1)) {
						TRACE("dropped CIRCUIT_TEST from %s(%s): previous hop COM invalid",source().toString().c_str(),_remoteAddress.toString().c_str());
						return true;
					}
				}	break;
				default: break;
			}
		}
		vlf += previousHopCredentialLength;

		// Check credentials (signature already verified)
		SharedPtr<NetworkConfig> originatorCredentialNetworkConfig;
		if (originatorCredentialNetworkId) {
			if (Network::controllerFor(originatorCredentialNetworkId) == originatorAddress) {
				SharedPtr<Network> nw(RR->node->network(originatorCredentialNetworkId));
				if (nw) {
					originatorCredentialNetworkConfig = nw->config2();
					if ( (originatorCredentialNetworkConfig) && ((originatorCredentialNetworkConfig->isPublic())||(peer->address() == originatorAddress)||((originatorCredentialNetworkConfig->com())&&(previousHopCom)&&(originatorCredentialNetworkConfig->com().agreesWith(previousHopCom)))) ) {
						TRACE("CIRCUIT_TEST %.16llx received from hop %s(%s) and originator %s with valid network ID credential %.16llx (verified from originator and next hop)",testId,source().toString().c_str(),_remoteAddress.toString().c_str(),originatorAddress.toString().c_str(),originatorCredentialNetworkId);
					} else {
						TRACE("dropped CIRCUIT_TEST from %s(%s): originator %s specified network ID %.16llx as credential, and previous hop %s did not supply a valid COM",source().toString().c_str(),_remoteAddress.toString().c_str(),originatorAddress.toString().c_str(),originatorCredentialNetworkId,peer->address().toString().c_str());
						return true;
					}
				} else {
					TRACE("dropped CIRCUIT_TEST from %s(%s): originator %s specified network ID %.16llx as credential, and we are not a member",source().toString().c_str(),_remoteAddress.toString().c_str(),originatorAddress.toString().c_str(),originatorCredentialNetworkId);
					return true;
				}
			} else {
				TRACE("dropped CIRCUIT_TEST from %s(%s): originator %s specified network ID as credential, is not controller for %.16llx",source().toString().c_str(),_remoteAddress.toString().c_str(),originatorAddress.toString().c_str(),originatorCredentialNetworkId);
				return true;
			}
		} else {
			TRACE("dropped CIRCUIT_TEST from %s(%s): originator %s did not specify a credential or credential type",source().toString().c_str(),_remoteAddress.toString().c_str(),originatorAddress.toString().c_str());
			return true;
		}

		const uint64_t now = RR->node->now();

		unsigned int breadth = 0;
		Address nextHop[256]; // breadth is a uin8_t, so this is the max
		InetAddress nextHopBestPathAddress[256];
		unsigned int remainingHopsPtr = ZT_PACKET_IDX_PAYLOAD + 33 + vlf;
		if ((ZT_PACKET_IDX_PAYLOAD + 31 + vlf) < size()) {
			// unsigned int nextHopFlags = (*this)[ZT_PACKET_IDX_PAYLOAD + 31 + vlf]
			breadth = (*this)[ZT_PACKET_IDX_PAYLOAD + 32 + vlf];
			for(unsigned int h=0;h<breadth;++h) {
				nextHop[h].setTo(field(remainingHopsPtr,ZT_ADDRESS_LENGTH),ZT_ADDRESS_LENGTH);
				remainingHopsPtr += ZT_ADDRESS_LENGTH;
				SharedPtr<Peer> nhp(RR->topology->getPeer(nextHop[h]));
				if (nhp) {
					RemotePath *const rp = nhp->getBestPath(now);
					if (rp)
						nextHopBestPathAddress[h] = rp->address();
				}
			}
		}

		// Report back to originator, depending on flags and whether we are last hop
		if ( ((flags & 0x01) != 0) || ((breadth == 0)&&((flags & 0x02) != 0)) ) {
			Packet outp(originatorAddress,RR->identity.address(),Packet::VERB_CIRCUIT_TEST_REPORT);
			outp.append((uint64_t)timestamp);
			outp.append((uint64_t)testId);
			outp.append((uint64_t)now);
			outp.append((uint8_t)ZT_VENDOR_ZEROTIER);
			outp.append((uint8_t)ZT_PROTO_VERSION);
			outp.append((uint8_t)ZEROTIER_ONE_VERSION_MAJOR);
			outp.append((uint8_t)ZEROTIER_ONE_VERSION_MINOR);
			outp.append((uint16_t)ZEROTIER_ONE_VERSION_REVISION);
			outp.append((uint16_t)ZT_PLATFORM_UNSPECIFIED);
			outp.append((uint16_t)ZT_ARCHITECTURE_UNSPECIFIED);
			outp.append((uint16_t)0); // error code, currently unused
			outp.append((uint64_t)0); // flags, currently unused
			outp.append((uint64_t)packetId());
			peer->address().appendTo(outp);
			outp.append((uint8_t)hops());
			_localAddress.serialize(outp);
			_remoteAddress.serialize(outp);
			outp.append((uint16_t)0); // no additional fields
			outp.append((uint8_t)breadth);
			for(unsigned int h=0;h<breadth;++h) {
				nextHop[h].appendTo(outp);
				nextHopBestPathAddress[h].serialize(outp); // appends 0 if null InetAddress
			}
			RR->sw->send(outp,true,0);
		}

		// If there are next hops, forward the test along through the graph
		if (breadth > 0) {
			Packet outp(Address(),RR->identity.address(),Packet::VERB_CIRCUIT_TEST);
			outp.append(field(ZT_PACKET_IDX_PAYLOAD,lengthOfSignedPortionAndSignature),lengthOfSignedPortionAndSignature);
			const unsigned int previousHopCredentialPos = outp.size();
			outp.append((uint16_t)0); // no previous hop credentials: default
			if ((originatorCredentialNetworkConfig)&&(!originatorCredentialNetworkConfig->isPublic())&&(originatorCredentialNetworkConfig->com())) {
				outp.append((uint8_t)0x01); // COM
				originatorCredentialNetworkConfig->com().serialize(outp);
				outp.setAt<uint16_t>(previousHopCredentialPos,(uint16_t)(size() - previousHopCredentialPos));
			}
			if (remainingHopsPtr < size())
				outp.append(field(remainingHopsPtr,size() - remainingHopsPtr),size() - remainingHopsPtr);

			for(unsigned int h=0;h<breadth;++h) {
				if (RR->identity.address() != nextHop[h]) { // next hops that loop back to the current hop are not valid
					outp.newInitializationVector();
					outp.setDestination(nextHop[h]);
					RR->sw->send(outp,true,originatorCredentialNetworkId);
				}
			}
		}

		peer->received(RR,_localAddress,_remoteAddress,hops(),packetId(),Packet::VERB_CIRCUIT_TEST,0,Packet::VERB_NOP);
	} catch ( ... ) {
		TRACE("dropped CIRCUIT_TEST from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doCIRCUIT_TEST_REPORT(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		ZT_CircuitTestReport report;
		memset(&report,0,sizeof(report));

		report.current = peer->address().toInt();
		report.upstream = Address(field(ZT_PACKET_IDX_PAYLOAD + 52,ZT_ADDRESS_LENGTH),ZT_ADDRESS_LENGTH).toInt();
		report.testId = at<uint64_t>(ZT_PACKET_IDX_PAYLOAD + 8);
		report.timestamp = at<uint64_t>(ZT_PACKET_IDX_PAYLOAD);
		report.remoteTimestamp = at<uint64_t>(ZT_PACKET_IDX_PAYLOAD + 16);
		report.sourcePacketId = at<uint64_t>(ZT_PACKET_IDX_PAYLOAD + 44);
		report.flags = at<uint64_t>(ZT_PACKET_IDX_PAYLOAD + 36);
		report.sourcePacketHopCount = (*this)[ZT_PACKET_IDX_PAYLOAD + 57]; // end of fixed length headers: 58
		report.errorCode = at<uint16_t>(ZT_PACKET_IDX_PAYLOAD + 34);
		report.vendor = (enum ZT_Vendor)((*this)[ZT_PACKET_IDX_PAYLOAD + 24]);
		report.protocolVersion = (*this)[ZT_PACKET_IDX_PAYLOAD + 25];
		report.majorVersion = (*this)[ZT_PACKET_IDX_PAYLOAD + 26];
		report.minorVersion = (*this)[ZT_PACKET_IDX_PAYLOAD + 27];
		report.revision = at<uint16_t>(ZT_PACKET_IDX_PAYLOAD + 28);
		report.platform = (enum ZT_Platform)at<uint16_t>(ZT_PACKET_IDX_PAYLOAD + 30);
		report.architecture = (enum ZT_Architecture)at<uint16_t>(ZT_PACKET_IDX_PAYLOAD + 32);

		const unsigned int receivedOnLocalAddressLen = reinterpret_cast<InetAddress *>(&(report.receivedOnLocalAddress))->deserialize(*this,ZT_PACKET_IDX_PAYLOAD + 58);
		const unsigned int receivedFromRemoteAddressLen = reinterpret_cast<InetAddress *>(&(report.receivedFromRemoteAddress))->deserialize(*this,ZT_PACKET_IDX_PAYLOAD + 58 + receivedOnLocalAddressLen);

		unsigned int nhptr = ZT_PACKET_IDX_PAYLOAD + 58 + receivedOnLocalAddressLen + receivedFromRemoteAddressLen;
		nhptr += at<uint16_t>(nhptr) + 2; // add "additional field" length, which right now will be zero

		report.nextHopCount = (*this)[nhptr++];
		if (report.nextHopCount > ZT_CIRCUIT_TEST_MAX_HOP_BREADTH) // sanity check, shouldn't be possible
			report.nextHopCount = ZT_CIRCUIT_TEST_MAX_HOP_BREADTH;
		for(unsigned int h=0;h<report.nextHopCount;++h) {
			report.nextHops[h].address = Address(field(nhptr,ZT_ADDRESS_LENGTH),ZT_ADDRESS_LENGTH).toInt(); nhptr += ZT_ADDRESS_LENGTH;
			nhptr += reinterpret_cast<InetAddress *>(&(report.nextHops[h].physicalAddress))->deserialize(*this,nhptr);
		}

		RR->node->postCircuitTestReport(&report);
	} catch ( ... ) {
		TRACE("dropped CIRCUIT_TEST_REPORT from %s(%s): unexpected exception",source().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

bool IncomingPacket::_doREQUEST_PROOF_OF_WORK(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer)
{
	try {
		// Right now this is only allowed from root servers -- may be allowed from controllers and relays later.
		if (RR->topology->isRoot(peer->identity())) {
			const uint64_t pid = packetId();
			const unsigned int difficulty = (*this)[ZT_PACKET_IDX_PAYLOAD + 1];
			const unsigned int challengeLength = at<uint16_t>(ZT_PACKET_IDX_PAYLOAD + 2);
			if (challengeLength > ZT_PROTO_MAX_PACKET_LENGTH)
				return true; // sanity check, drop invalid size
			const unsigned char *challenge = field(ZT_PACKET_IDX_PAYLOAD + 4,challengeLength);

			switch((*this)[ZT_PACKET_IDX_PAYLOAD]) {

				// Salsa20/12+SHA512 hashcash
				case 0x01: {
					if (difficulty <= 14) {
						unsigned char result[16];
						computeSalsa2012Sha512ProofOfWork(difficulty,challenge,challengeLength,result);
						TRACE("PROOF_OF_WORK computed for %s: difficulty==%u, challengeLength==%u, result: %.16llx%.16llx",peer->address().toString().c_str(),difficulty,challengeLength,Utils::ntoh(*(reinterpret_cast<const uint64_t *>(result))),Utils::ntoh(*(reinterpret_cast<const uint64_t *>(result + 8))));
						Packet outp(peer->address(),RR->identity.address(),Packet::VERB_OK);
						outp.append((unsigned char)Packet::VERB_REQUEST_PROOF_OF_WORK);
						outp.append(pid);
						outp.append((uint16_t)sizeof(result));
						outp.append(result,sizeof(result));
						outp.armor(peer->key(),true);
						RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
					} else {
						Packet outp(peer->address(),RR->identity.address(),Packet::VERB_ERROR);
						outp.append((unsigned char)Packet::VERB_REQUEST_PROOF_OF_WORK);
						outp.append(pid);
						outp.append((unsigned char)Packet::ERROR_INVALID_REQUEST);
						outp.armor(peer->key(),true);
						RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
					}
				}	break;

				default:
					TRACE("dropped REQUEST_PROOF_OF_WORK from %s(%s): unrecognized proof of work type",peer->address().toString().c_str(),_remoteAddress.toString().c_str());
					break;
			}

			peer->received(RR,_localAddress,_remoteAddress,hops(),pid,Packet::VERB_REQUEST_PROOF_OF_WORK,0,Packet::VERB_NOP);
		} else {
			TRACE("dropped REQUEST_PROOF_OF_WORK from %s(%s): not trusted enough",peer->address().toString().c_str(),_remoteAddress.toString().c_str());
		}
	} catch ( ... ) {
		TRACE("dropped REQUEST_PROOF_OF_WORK from %s(%s): unexpected exception",peer->address().toString().c_str(),_remoteAddress.toString().c_str());
	}
	return true;
}

void IncomingPacket::computeSalsa2012Sha512ProofOfWork(unsigned int difficulty,const void *challenge,unsigned int challengeLength,unsigned char result[16])
{
	unsigned char salsabuf[131072]; // 131072 == protocol constant, size of memory buffer for this proof of work function
	char candidatebuf[ZT_PROTO_MAX_PACKET_LENGTH + 256];
	unsigned char shabuf[ZT_SHA512_DIGEST_LEN];
	const uint64_t s20iv = 0; // zero IV for Salsa20
	char *const candidate = (char *)(( ((uintptr_t)&(candidatebuf[0])) | 0xf ) + 1); // align to 16-byte boundary to ensure that uint64_t type punning of initial nonce is okay
	Salsa20 s20;
	unsigned int d;
	unsigned char *p;

	Utils::getSecureRandom(candidate,16);
	memcpy(candidate + 16,challenge,challengeLength);

	if (difficulty > 512)
		difficulty = 512; // sanity check

try_salsa2012sha512_again:
	++*(reinterpret_cast<volatile uint64_t *>(candidate));

	SHA512::hash(shabuf,candidate,16 + challengeLength);
	s20.init(shabuf,256,&s20iv);
	memset(salsabuf,0,sizeof(salsabuf));
	s20.encrypt12(salsabuf,salsabuf,sizeof(salsabuf));
	SHA512::hash(shabuf,salsabuf,sizeof(salsabuf));

	d = difficulty;
	p = shabuf;
	while (d >= 8) {
		if (*(p++))
			goto try_salsa2012sha512_again;
		d -= 8;
	}
	if (d > 0) {
		if ( ((((unsigned int)*p) << d) & 0xff00) != 0 )
			goto try_salsa2012sha512_again;
	}

	memcpy(result,candidate,16);
}

bool IncomingPacket::testSalsa2012Sha512ProofOfWorkResult(unsigned int difficulty,const void *challenge,unsigned int challengeLength,const unsigned char proposedResult[16])
{
	unsigned char salsabuf[131072]; // 131072 == protocol constant, size of memory buffer for this proof of work function
	char candidate[ZT_PROTO_MAX_PACKET_LENGTH + 256];
	unsigned char shabuf[ZT_SHA512_DIGEST_LEN];
	const uint64_t s20iv = 0; // zero IV for Salsa20
	Salsa20 s20;
	unsigned int d;
	unsigned char *p;

	if (difficulty > 512)
		difficulty = 512; // sanity check

	memcpy(candidate,proposedResult,16);
	memcpy(candidate + 16,challenge,challengeLength);

	SHA512::hash(shabuf,candidate,16 + challengeLength);
	s20.init(shabuf,256,&s20iv);
	memset(salsabuf,0,sizeof(salsabuf));
	s20.encrypt12(salsabuf,salsabuf,sizeof(salsabuf));
	SHA512::hash(shabuf,salsabuf,sizeof(salsabuf));

	d = difficulty;
	p = shabuf;
	while (d >= 8) {
		if (*(p++))
			return false;
		d -= 8;
	}
	if (d > 0) {
		if ( ((((unsigned int)*p) << d) & 0xff00) != 0 )
			return false;
	}

	return true;
}

void IncomingPacket::_sendErrorNeedCertificate(const RuntimeEnvironment *RR,const SharedPtr<Peer> &peer,uint64_t nwid)
{
	Packet outp(source(),RR->identity.address(),Packet::VERB_ERROR);
	outp.append((unsigned char)verb());
	outp.append(packetId());
	outp.append((unsigned char)Packet::ERROR_NEED_MEMBERSHIP_CERTIFICATE);
	outp.append(nwid);
	outp.armor(peer->key(),true);
	RR->node->putPacket(_localAddress,_remoteAddress,outp.data(),outp.size());
}

} // namespace ZeroTier
