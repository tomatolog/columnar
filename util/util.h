// Copyright (c) 2020-2021, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <type_traits>
#include <fcntl.h>
#include <climits>

namespace columnar
{

#ifdef _MSC_VER
	#define DLLEXPORT __declspec(dllexport)
#else
	#define O_BINARY 0
	#define DLLEXPORT
#endif

#ifndef FORCE_INLINE
	#ifndef NDEBUG
		#define FORCE_INLINE inline
	#else
		#ifdef _MSC_VER
			#define FORCE_INLINE __forceinline
		#else
			#if defined (__cplusplus) || defined (__STDC_VERSION__) && __STDC_VERSION__ >= 199901L   /* C99 */
				#ifdef __GNUC__
					#define FORCE_INLINE inline __attribute__((always_inline))
				#else
					#define FORCE_INLINE inline
				#endif
			#else
				#define FORCE_INLINE
			#endif
		#endif
	#endif
#endif


template<typename T>
class Span_T
{
public:
	Span_T() = default;

	Span_T ( T * pData, size_t tLength )
		: m_pData ( pData )
		, m_tLength ( tLength )
	{}

	Span_T ( std::vector<T> & dVec )
		: m_pData ( dVec.data() )
		, m_tLength ( dVec.size() )
	{}

	T *     data() const    { return m_pData; }
	T &     front() const   { return *m_pData; }
	T &     back() const    { return *(m_pData+m_tLength-1); }
	T *     begin() const   { return m_pData; }
	T *     end() const     { return m_pData+m_tLength; }
	size_t  size() const    { return m_tLength; }
	bool    empty() const   { return m_tLength==0; }
	T & operator [] ( size_t i ) { return m_pData[i]; }
	const T & operator [] ( size_t i ) const { return m_pData[i]; }

protected:
	T *		m_pData = nullptr;
	size_t	m_tLength = 0; 
};


template<typename T>
class SpanResizeable_T : public Span_T<T>
{
	using BASE = Span_T<T>;

public:
	void resize ( size_t tLength )
	{
		if ( tLength>m_tMaxLength )
		{
			m_tMaxLength = tLength;
			m_dData.resize(m_tMaxLength);
			BASE::m_pData = m_dData.data();
		}

		BASE::m_tLength = tLength;
	}

private:
	std::vector<T>	m_dData;
	size_t			m_tMaxLength = 0;
};


class ByteCodec_c
{
public:
	template <typename T>
	static inline uint8_t * PackData ( const Span_T<T> & dData )
	{
		size_t tSizeBytes = dData.size()*sizeof(T);
		uint8_t * pResult = nullptr;
		uint8_t * pData = nullptr;
		std::tie ( pResult, pData ) = PackData(tSizeBytes);
		memcpy ( pData, dData.begin(), tSizeBytes );
		return pResult;
	}

	static inline std::pair<uint8_t *,uint8_t *> PackData ( size_t tDataLen )
	{
		uint8_t dPacked[16];
		uint8_t * pPacked = dPacked;
		int iPackedLen = Pack_uint64 ( pPacked, tDataLen );
		uint8_t * pResult = new uint8_t[iPackedLen + tDataLen];
		memcpy ( pResult, dPacked, iPackedLen );
		return { pResult, pResult+iPackedLen };
	}

	static inline void PackData ( std::vector<uint8_t> & dPacked, const Span_T<uint8_t> & dData )
	{
		dPacked.resize ( dData.size() + CalcPackedLen (dData.size() ) );
		uint8_t * p = dPacked.data();
		Pack_uint64 ( p, dData.size() );
		memcpy ( p, dData.begin(), dData.size() );
	}

	template <typename T>
	static inline int CalcPackedLen ( T tValue )
	{
		int iNumBytes = 1;
		uint64_t uValue = (uint64_t)tValue;
		uValue >>= 7;
		while ( uValue )
		{
			uValue >>= 7;
			iNumBytes++;
		}

		return iNumBytes;
	}

	static inline int Pack_uint32 ( uint8_t * & pOut, uint32_t uValue )
	{
		return EncodeValue ( pOut, uValue );
	}

	static inline int Pack_uint64 ( uint8_t * & pOut, uint64_t uValue )
	{
		return EncodeValue ( pOut, uValue );
	}

	template <typename GET>
	static inline uint32_t Unpack_uint32 ( GET && fnGetByte )
	{
		return DecodeValue<uint32_t>(fnGetByte);
	}

	template <typename GET>
	static inline uint64_t Unpack_uint64 ( GET && fnGetByte )
	{
		return DecodeValue<uint64_t>(fnGetByte);
	}

	template <typename T>
	static inline int EncodeValue ( uint8_t * & pOut, T tValue )
	{
		int iNumBytes = CalcPackedLen(tValue);
		for ( int i = iNumBytes-1; i>=0; i-- )
		{
			uint8_t uEncoded = ( tValue >> ( 7*i ) ) & 0x7f;
			if ( i )
				uEncoded |= 0x80;

			*pOut++ = uEncoded;
		}

		return iNumBytes;
	}

private:
	template <typename T, typename GET>
	static inline T DecodeValue ( GET && fnGetByte )
	{
		uint8_t uByte = fnGetByte();
		T tValue = 0;
		while ( uByte & 0x80 )
		{
			tValue = ( tValue << 7 ) | ( uByte & 0x7f );
			uByte = fnGetByte();
		}

		return ( tValue << 7 ) | uByte;
	}
};


class FileWriter_c
{
public:
				~FileWriter_c();

	bool        Open ( const std::string & sFile, bool bNewFile, bool bAppend, bool bTmp, std::string & sError );
	bool        Open ( const std::string & sFile, std::string & sError );
	void        Close();
	void        Unlink();
	std::string GetFilename() const { return m_sFile; }

	void        Write ( const uint8_t * pData, size_t tLength );
	void        SeekAndWrite ( int64_t iOffset, uint64_t uValue );
	void        Seek ( int64_t iOffset );
	void        Write_string ( const std::string & sStr );
	void        Write_uint8 ( uint8_t uValue )      { Write ( (uint8_t*)&uValue, sizeof(uValue) ); }
	void        Write_uint16 ( uint16_t uValue )    { Write ( (uint8_t*)&uValue, sizeof(uValue) ); }
	void        Write_uint32 ( uint32_t uValue )    { Write ( (uint8_t*)&uValue, sizeof(uValue) ); }
	void        Write_uint64 ( uint64_t uValue )    { Write ( (uint8_t*)&uValue, sizeof(uValue) ); }

	void        Pack_uint32 ( uint32_t uValue ) { PackValue(uValue); }
	void        Pack_uint64 ( uint64_t uValue ) { PackValue(uValue); }

	int64_t     GetPos() const { return m_iFilePos + (int64_t)m_tUsed; }
	bool        IsError() const { return m_bError; }
	std::string GetError() const { return m_sError; }

private:
	static const size_t DEFAULT_SIZE = 1048576;

	int         m_iFD = -1;
	std::string m_sFile;

	std::unique_ptr<uint8_t[]> m_pData;

	size_t      m_tSize = DEFAULT_SIZE;
	size_t      m_tUsed = 0;

	int64_t     m_iFilePos = 0;

	bool        m_bError = false;
	bool		m_bTemporary = false; // whatever to unlink file at writer destructor
	std::string m_sError;

	void        Flush();

	template <typename T>
	void PackValue ( T uValue )
	{
		uint8_t dPacked[16];
		uint8_t * pOut = dPacked;
		int iPackedLen = ByteCodec_c::EncodeValue ( pOut, uValue );
		Write ( dPacked, iPackedLen );
	}
};


class MemWriter_c
{
public:
			MemWriter_c ( std::vector<uint8_t> & dData );

	void    Write ( const uint8_t * pData, size_t tSize );
	int64_t GetPos() const { return (int64_t)m_dData.size(); }

	void    Write_uint8 ( uint8_t uValue ) { m_dData.push_back(uValue); }
	void    Write_uint16 ( uint16_t uValue ) { WriteValue(uValue); }
	void    Write_uint32 ( uint64_t uValue ) { WriteValue(uValue); }
	void    Write_uint64 ( uint64_t uValue ) { WriteValue(uValue); }

	void    Pack_uint32 ( uint32_t uValue ) { PackValue(uValue); }
	void    Pack_uint64 ( uint64_t uValue ) { PackValue(uValue); }

private:
	std::vector<uint8_t> & m_dData;

	template <typename T>
	void    WriteValue ( T tValue ) { Write ( (const uint8_t*)&tValue, sizeof(tValue) ); }

	template <typename T>
	void PackValue ( T uValue )
	{
		uint8_t dPacked[16];
		uint8_t * pOut = dPacked;
		int iPackedLen = ByteCodec_c::EncodeValue ( pOut, uValue );
		size_t tOldSize = m_dData.size();
		m_dData.resize ( tOldSize+iPackedLen );
		memcpy ( &m_dData[tOldSize], dPacked, iPackedLen );
	}
};


template<typename ... Args>
std::string FormatStr ( const std::string & sFormat, Args ... args )
{
	int iSize = snprintf ( nullptr, 0, sFormat.c_str(), args ... ) + 1;
	if ( iSize<=0 )
		return "";

	std::unique_ptr<char[]> pBuf( new char[iSize] );
	snprintf ( pBuf.get(), iSize, sFormat.c_str(), args ... );
	return std::string ( pBuf.get(), pBuf.get()+iSize-1 );
}


inline uint32_t FloatToUint ( float fValue )
{
	union { float m_fValue; uint32_t m_uValue; } tUnion;
	tUnion.m_fValue = fValue;
	return tUnion.m_uValue;
}

inline float UintToFloat ( uint32_t uValue )
{
	union { float m_fValue; uint32_t m_uValue; } tUnion;
	tUnion.m_uValue = uValue;
	return tUnion.m_fValue;
}

template<class T, class CONTAINER>
const T * binary_search ( const CONTAINER & dValues, const T & tValue )
{
	auto tFirst = dValues.begin();
	auto tLast = dValues.end();
	auto tFound = std::lower_bound ( tFirst, tLast, tValue );
	if ( tFound==tLast || tValue < *tFound )
		return nullptr;

	return &(*tFound);
}

template<typename VEC, typename T>
int binary_search_idx ( const VEC & dValues, const T & tValue )
{
	auto tFirst = dValues.begin();
	auto tLast = dValues.end();
	auto tFound = std::lower_bound ( tFirst, tLast, tValue );

	if ( tFound!=tLast && tValue==*tFound )
		return tFound-tFirst;

	return -1;
}

template <typename T>
constexpr auto to_underlying(T t) noexcept
{
	return static_cast<typename std::underlying_type<T>::type>(t);
}

template <typename T>
inline T to_type ( int64_t iValue )
{
	return (T)iValue;
}

template <>
inline float to_type<float> ( int64_t iValue )
{
	return UintToFloat ( (uint32_t)iValue );
}

int     CalcNumBits ( uint64_t uNumber );
bool    CopySingleFile ( const std::string & sSource, const std::string & sDest, std::string & sError, int iMode );

template<typename VEC>
void VectorReset ( VEC & dData )
{
	dData.clear();
	dData.shrink_to_fit();
}

template<typename VEC, typename WRITER>
void WriteVector ( const VEC & dData, WRITER & tWriter )
{
	tWriter.Write ( (const uint8_t *)dData.data(), sizeof ( dData[0] ) * dData.size() );
}

template<typename VEC, typename WRITER>
void WriteVectorLen ( const VEC & dData, WRITER & tWriter )
{
	tWriter.Pack_uint64 ( dData.size() );
	tWriter.Write ( (const uint8_t *)dData.data(), sizeof ( dData[0] ) * dData.size() );
}

template<typename VEC, typename WRITER>
void WriteVectorLen32 ( const VEC & dData, WRITER & tWriter )
{
	tWriter.Pack_uint32 ( (uint32_t)dData.size() );
	tWriter.Write ( (const uint8_t *)dData.data(), sizeof ( dData[0] ) * dData.size() );
}

template<typename VEC, typename WRITER>
void WriteVectorRawLen32 ( const VEC & dData, WRITER & tWriter )
{
	tWriter.Write_uint32 ( (uint32_t)dData.size() );
	tWriter.Write ( (const uint8_t *)dData.data(), sizeof ( dData[0] ) * dData.size() );
}

template<typename WRITER>
void WriteVectorPacked ( const std::vector<uint64_t> & dData, WRITER & tWriter )
{
	tWriter.Pack_uint32 ( (uint32_t)dData.size() );
	for ( uint64_t tVal : dData )
		tWriter.Pack_uint64 ( tVal );
}

bool FloatEqual ( float fA, float fB );

struct BitVec_t
{
	std::vector<uint32_t> m_dData;
	int m_iSize { 0 };

	explicit BitVec_t ( int iSize );

	bool BitGet ( int iBit );
	void BitSet ( int iBit );
};

/// known collations
enum class Collation_e : uint32_t
{
	LIBC_CI,
	LIBC_CS,
	UTF8_GENERAL_CI,
	BINARY,

	TOTAL
};

} // namespace columnar
