#include <string.h>
#include "I2PEndian.h"
#include "Crypto.h"
#include "Log.h"
#include "Timestamp.h"
#include "NetDb.hpp"
#include "Tunnel.h"
#include "LeaseSet.h"

namespace i2p
{
namespace data
{

	LeaseSet::LeaseSet ():
		m_IsValid (false), m_StoreLeases (false), m_ExpirationTime (0), m_Buffer (nullptr), m_BufferLen (0)
	{
	}

	LeaseSet::LeaseSet (const uint8_t * buf, size_t len, bool storeLeases):
		m_IsValid (true), m_StoreLeases (storeLeases), m_ExpirationTime (0)
	{
		m_Buffer = new uint8_t[len];
		memcpy (m_Buffer, buf, len);
		m_BufferLen = len;
		ReadFromBuffer ();
	}

	void LeaseSet::Update (const uint8_t * buf, size_t len, bool verifySignature)
	{
		if (len > m_BufferLen)
		{
			auto oldBuffer = m_Buffer;
			m_Buffer = new uint8_t[len];
			delete[] oldBuffer;
		}
		memcpy (m_Buffer, buf, len);
		m_BufferLen = len;
		ReadFromBuffer (false, verifySignature);
	}

	void LeaseSet::PopulateLeases ()
	{
		m_StoreLeases = true;
		ReadFromBuffer (false);
	}

	void LeaseSet::ReadFromBuffer (bool readIdentity, bool verifySignature)
	{
		if (readIdentity || !m_Identity)
			m_Identity = std::make_shared<IdentityEx>(m_Buffer, m_BufferLen);
		size_t size = m_Identity->GetFullLen ();
		if (size > m_BufferLen)
		{
			LogPrint (eLogError, "LeaseSet: identity length ", size, " exceeds buffer size ", m_BufferLen);
			m_IsValid = false;
			return;
		}
		memcpy (m_EncryptionKey, m_Buffer + size, 256);
		size += 256; // encryption key
		size += m_Identity->GetSigningPublicKeyLen (); // unused signing key
		uint8_t num = m_Buffer[size];
		size++; // num
		LogPrint (eLogDebug, "LeaseSet: read num=", (int)num);
		if (!num || num > MAX_NUM_LEASES)
		{
			LogPrint (eLogError, "LeaseSet: incorrect number of leases", (int)num);
			m_IsValid = false;
			return;
		}

		// reset existing leases
		if (m_StoreLeases)
			for (auto& it: m_Leases)
				it->isUpdated = false;
		else
			m_Leases.clear ();

		// process leases
		m_ExpirationTime = 0;
		auto ts = i2p::util::GetMillisecondsSinceEpoch ();
		const uint8_t * leases = m_Buffer + size;
		for (int i = 0; i < num; i++)
		{
			Lease lease;
			lease.tunnelGateway = leases;
			leases += 32; // gateway
			lease.tunnelID = bufbe32toh (leases);
			leases += 4; // tunnel ID
			lease.endDate = bufbe64toh (leases);
			leases += 8; // end date
			UpdateLease (lease, ts);
		}
		if (!m_ExpirationTime)
		{
			LogPrint (eLogWarning, "LeaseSet: all leases are expired. Dropped");
			m_IsValid = false;
			return;
		}
		m_ExpirationTime += LEASE_ENDDATE_THRESHOLD;
		// delete old leases
		if (m_StoreLeases)
		{
			for (auto it = m_Leases.begin (); it != m_Leases.end ();)
			{
				if (!(*it)->isUpdated)
				{
					(*it)->endDate = 0; // somebody might still hold it
					m_Leases.erase (it++);
				}
				else
					++it;
			}
		}

		// verify
		if (verifySignature && !m_Identity->Verify (m_Buffer, leases - m_Buffer, leases))
		{
			LogPrint (eLogWarning, "LeaseSet: verification failed");
			m_IsValid = false;
		}
	}

	void LeaseSet::UpdateLease (const Lease& lease, uint64_t ts)
	{
		if (ts < lease.endDate + LEASE_ENDDATE_THRESHOLD)
		{
			if (lease.endDate > m_ExpirationTime)
				m_ExpirationTime = lease.endDate;
			if (m_StoreLeases)
			{
				auto ret = m_Leases.insert (std::make_shared<Lease>(lease));
				if (!ret.second) (*ret.first)->endDate = lease.endDate; // update existing
				(*ret.first)->isUpdated = true;
				// check if lease's gateway is in our netDb
				if (!netdb.FindRouter (lease.tunnelGateway))
				{
					// if not found request it
					LogPrint (eLogInfo, "LeaseSet: Lease's tunnel gateway not found, requesting");
					netdb.RequestDestination (lease.tunnelGateway);
				}
			}
		}
		else
			LogPrint (eLogWarning, "LeaseSet: Lease is expired already ");
	}

	uint64_t LeaseSet::ExtractTimestamp (const uint8_t * buf, size_t len) const
	{
		if (!m_Identity) return 0;
		size_t size = m_Identity->GetFullLen ();
		if (size > len) return 0;
		size += 256; // encryption key
		size += m_Identity->GetSigningPublicKeyLen (); // unused signing key
		if (size > len) return 0;
		uint8_t num = buf[size];
		size++; // num
		if (size + num*LEASE_SIZE > len) return 0;
		uint64_t timestamp= 0 ;
		for (int i = 0; i < num; i++)
		{
			size += 36; // gateway (32) + tunnelId(4)
			auto endDate = bufbe64toh (buf + size);
			size += 8; // end date
			if (!timestamp || endDate < timestamp)
				timestamp = endDate;
		}
		return timestamp;
	}

	bool LeaseSet::IsNewer (const uint8_t * buf, size_t len) const
	{
		return ExtractTimestamp (buf, len) > ExtractTimestamp (m_Buffer, m_BufferLen);
	}

	bool LeaseSet::ExpiresSoon(const uint64_t dlt, const uint64_t fudge) const
	{
		auto now = i2p::util::GetMillisecondsSinceEpoch ();
		if (fudge) now += rand() % fudge;
		if (now >= m_ExpirationTime) return true;
		return	m_ExpirationTime - now <= dlt;
	}

  const std::vector<std::shared_ptr<const Lease> > LeaseSet::GetNonExpiredLeases (bool withThreshold) const
  {
    return GetNonExpiredLeasesExcluding( [] (const Lease & l) -> bool { return false; }, withThreshold);
  }

	const std::vector<std::shared_ptr<const Lease> > LeaseSet::GetNonExpiredLeasesExcluding (LeaseInspectFunc exclude, bool withThreshold) const
	{
		auto ts = i2p::util::GetMillisecondsSinceEpoch ();
		std::vector<std::shared_ptr<const Lease> > leases;
		for (const auto& it: m_Leases)
		{
			auto endDate = it->endDate;
			if (withThreshold)
				endDate += LEASE_ENDDATE_THRESHOLD;
			else
				endDate -= LEASE_ENDDATE_THRESHOLD;
			if (ts < endDate && !exclude(*it))
				leases.push_back (it);
		}
		return leases;
	}

	bool LeaseSet::HasExpiredLeases () const
	{
		auto ts = i2p::util::GetMillisecondsSinceEpoch ();
		for (const auto& it: m_Leases)
			if (ts >= it->endDate) return true;
		return false;
	}

	bool LeaseSet::IsExpired () const
	{
		if (m_StoreLeases && IsEmpty ()) return true;
		auto ts = i2p::util::GetMillisecondsSinceEpoch ();
		return ts > m_ExpirationTime;
	}

	void LeaseSet::Encrypt (const uint8_t * data, uint8_t * encrypted, BN_CTX * ctx) const
	{
		auto encryptor = m_Identity->CreateEncryptor (m_EncryptionKey);
		if (encryptor)
			encryptor->Encrypt (data, encrypted, ctx, true);
	}

	void LeaseSet::SetBuffer (const uint8_t * buf, size_t len)
	{
		if (m_Buffer) delete[] m_Buffer;
		m_Buffer = new uint8_t[len];
		m_BufferLen = len;
		memcpy (m_Buffer, buf, len);
	}

	LeaseSet2::LeaseSet2 (uint8_t storeType, const uint8_t * buf, size_t len):
		m_StoreType (storeType)
	{	
		SetBuffer (buf, len);
		ReadFromBuffer (buf, len);
	}

	void LeaseSet2::ReadFromBuffer (const uint8_t * buf, size_t len)
	{
		// standard LS2 header
		auto identity = std::make_shared<IdentityEx>(buf, len);
		SetIdentity (identity);
		size_t offset = identity->GetFullLen ();
		if (offset + 10 >= len) return;
		uint32_t timestamp = bufbe32toh (buf + offset); offset += 4; // published timestamp (seconds)
		uint16_t expires = bufbe16toh (buf + offset); offset += 2; // expires (seconds)
		SetExpirationTime ((timestamp + expires)*1000LL); // in milliseconds
		uint16_t flags = bufbe16toh (buf + offset); offset += 2; // flags
		std::unique_ptr<i2p::crypto::Verifier> offlineVerifier;
		if (flags & 0x0001)
		{
			// offline key
			if (offset + 6 >= len) return;
			const uint8_t * signedData = buf + offset;
			offset += 4; // expires timestamp
			uint16_t keyType = bufbe16toh (buf + offset); offset += 2;
			offlineVerifier.reset (i2p::data::IdentityEx::CreateVerifier (keyType));
			if (!offlineVerifier) return;
			auto keyLen = offlineVerifier->GetPublicKeyLen ();
			if (offset + keyLen >= len) return;
			offlineVerifier->SetPublicKey (buf + offset); offset += keyLen;
			if (offset + offlineVerifier->GetSignatureLen () >= len) return;
			if (!identity->Verify (signedData, keyLen + 6, buf + offset)) return;
			offset += offlineVerifier->GetSignatureLen ();
		}
		// type specific part
		size_t s = 0;
		switch (m_StoreType)
		{
			case NETDB_STORE_TYPE_STANDARD_LEASESET2:
				s = ReadStandardLS2TypeSpecificPart (buf + offset, len - offset);
			break;
			case NETDB_STORE_TYPE_META_LEASESET2:
				s = ReadMetaLS2TypeSpecificPart (buf + offset, len - offset);
			break;
			default:
				LogPrint (eLogWarning, "LeaseSet2: Unexpected store type ", (int)m_StoreType);
		}
		if (!s) return;
		offset += s;
		// verify signature
		if (offset + identity->GetSignatureLen () > len) return;
		uint8_t * buf1 = new uint8_t[offset + 1];
		buf1[0] = m_StoreType;
		memcpy (buf1 + 1, buf, offset); // TODO: implement it better
		bool verified = offlineVerifier ? offlineVerifier->Verify (buf1, offset + 1, buf + offset) : identity->Verify (buf1, offset + 1, buf + offset); 
		delete[] buf1;
		if (!verified)
			LogPrint (eLogWarning, "LeaseSet2: verification failed");
		SetIsValid (verified);	
	}

	size_t LeaseSet2::ReadStandardLS2TypeSpecificPart (const uint8_t * buf, size_t len)
	{
		size_t offset = 0;
		// properties
		uint16_t propertiesLen = bufbe16toh (buf + offset); offset += 2; 
		offset += propertiesLen; // skip for now. TODO: implement properties
		if (offset + 1 >= len) return 0;
		// key sections
		int numKeySections = buf[offset]; offset++;
		for (int i = 0; i < numKeySections; i++)
		{
			// skip key for now. TODO: implement encryption key
			offset += 2; // encryption key type
			if (offset + 2 >= len) return 0;
			uint16_t encryptionKeyLen = bufbe16toh (buf + offset); offset += 2; 
			offset += encryptionKeyLen; 
			if (offset >= len) return 0;
		}	
		// leases
		if (offset + 1 >= len) return 0;	
		int numLeases = buf[offset]; offset++;
		auto ts = i2p::util::GetMillisecondsSinceEpoch ();
		for (int i = 0; i < numLeases; i++)
		{
			if (offset + 40 > len) return 0;
			Lease lease;
			lease.tunnelGateway = buf + offset; offset += 32; // gateway
			lease.tunnelID = bufbe32toh (buf + offset); offset += 4; // tunnel ID
			lease.endDate = bufbe32toh (buf + offset)*1000LL; offset += 4; // end date
			UpdateLease (lease, ts);
		}
		return offset;
	}

	size_t LeaseSet2::ReadMetaLS2TypeSpecificPart (const uint8_t * buf, size_t len)
	{
		size_t offset = 0;
		// properties
		uint16_t propertiesLen = bufbe16toh (buf + offset); offset += 2; 
		offset += propertiesLen; // skip for now. TODO: implement properties
		// entries			
		if (offset + 1 >= len) return 0;
		int numEntries = buf[offset]; offset++;
		for (int i = 0; i < numEntries; i++)
		{
			if (offset + 40 >= len) return 0;
			offset += 32; // hash
			offset += 3; // flags
 			offset += 1; // cost
			offset += 4; // expires
		}
		// revocations
		if (offset + 1 >= len) return 0;
		int numRevocations = buf[offset]; offset++;	
		for (int i = 0; i < numRevocations; i++)
		{
			if (offset + 32 > len) return 0;
			offset += 32; // hash
		}
		return offset;
	}

	LocalLeaseSet::LocalLeaseSet (std::shared_ptr<const IdentityEx> identity, const uint8_t * encryptionPublicKey, std::vector<std::shared_ptr<i2p::tunnel::InboundTunnel> > tunnels):
		m_ExpirationTime (0), m_Identity (identity)
	{
		int num = tunnels.size ();
		if (num > MAX_NUM_LEASES) num = MAX_NUM_LEASES;
		// identity
		auto signingKeyLen = m_Identity->GetSigningPublicKeyLen ();
		m_BufferLen = m_Identity->GetFullLen () + 256 + signingKeyLen + 1 + num*LEASE_SIZE + m_Identity->GetSignatureLen ();
		m_Buffer = new uint8_t[m_BufferLen];
		auto offset = m_Identity->ToBuffer (m_Buffer, m_BufferLen);
		memcpy (m_Buffer + offset, encryptionPublicKey, 256);
		offset += 256;
		memset (m_Buffer + offset, 0, signingKeyLen);
		offset += signingKeyLen;
		// num leases
		m_Buffer[offset] = num;
		offset++;
		// leases
		m_Leases = m_Buffer + offset;
		auto currentTime = i2p::util::GetMillisecondsSinceEpoch ();
		for (int i = 0; i < num; i++)
		{
			memcpy (m_Buffer + offset, tunnels[i]->GetNextIdentHash (), 32);
			offset += 32; // gateway id
			htobe32buf (m_Buffer + offset, tunnels[i]->GetNextTunnelID ());
			offset += 4; // tunnel id
			uint64_t ts = tunnels[i]->GetCreationTime () + i2p::tunnel::TUNNEL_EXPIRATION_TIMEOUT - i2p::tunnel::TUNNEL_EXPIRATION_THRESHOLD; // 1 minute before expiration
			ts *= 1000; // in milliseconds
			if (ts > m_ExpirationTime) m_ExpirationTime = ts;
			// make sure leaseset is newer than previous, but adding some time to expiration date
			ts += (currentTime - tunnels[i]->GetCreationTime ()*1000LL)*2/i2p::tunnel::TUNNEL_EXPIRATION_TIMEOUT; // up to 2 secs
			htobe64buf (m_Buffer + offset, ts);
			offset += 8; // end date
		}
		//  we don't sign it yet. must be signed later on
	}

	LocalLeaseSet::LocalLeaseSet (std::shared_ptr<const IdentityEx> identity, const uint8_t * buf, size_t len):
		m_ExpirationTime (0), m_Identity (identity)
	{
		m_BufferLen = len;
		m_Buffer = new uint8_t[m_BufferLen];
		memcpy (m_Buffer, buf, len);
	}

	bool LocalLeaseSet::IsExpired () const
	{
		auto ts = i2p::util::GetMillisecondsSinceEpoch ();
		return ts > m_ExpirationTime;
	}

	bool LeaseSetBufferValidate(const uint8_t * ptr, size_t sz, uint64_t & expires)
	{
		IdentityEx ident(ptr, sz);
		size_t size = ident.GetFullLen ();
		if (size > sz)
		{
			LogPrint (eLogError, "LeaseSet: identity length ", size, " exceeds buffer size ", sz);
			return false;
		}
		// encryption key
		size += 256;
		// signing key (unused)
		size += ident.GetSigningPublicKeyLen ();
		uint8_t numLeases = ptr[size];
		++size;
		if (!numLeases || numLeases > MAX_NUM_LEASES)
		{
			LogPrint (eLogError, "LeaseSet: incorrect number of leases", (int)numLeases);
			return false;
		}
		const uint8_t * leases = ptr + size;
		expires = 0;
		/** find lease with the max expiration timestamp */
		for (int i = 0; i < numLeases; i++)
		{
			leases += 36; // gateway + tunnel ID
			uint64_t endDate = bufbe64toh (leases);
			leases += 8; // end date
			if(endDate > expires)
				expires = endDate;
		}
		return ident.Verify(ptr, leases - ptr, leases);
	}
}
}
