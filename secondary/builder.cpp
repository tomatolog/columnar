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

#include "sidx.h"
#include "codec.h"
#include "delta.h"
#include "pgm.h"

#include <queue>

// FastPFOR
#include "fastpfor.h"

#ifdef _MSC_VER
	#include <io.h>
#else
	#include <unistd.h>
#endif

namespace SI
{
	using namespace columnar;

#define BUILD_PRINT_VALUES 0
#define VALUES_PER_BLOCK 128

struct SIWriter_i
{
	SIWriter_i() = default;
	virtual ~SIWriter_i() = default;

	virtual bool Setup ( const std::string & sSrcFile, uint64_t iFileSize, std::vector<uint64_t> & dOffset, std::string & sError ) = 0;
	virtual bool Process ( FileWriter_c & tDstFile, FileWriter_c & tTmpBlocksOff, const std::string & sPgmValuesName, std::string & sError ) = 0;

	std::vector<uint64_t> m_dOffset;
	std::vector<uint8_t> m_dPGM;
};

struct RawWriter_i
{
	RawWriter_i() = default;
	virtual ~RawWriter_i() = default;

	virtual bool Setup ( const char * sFile, int iAttr, AttrType_e eAttrType, Collation_e eCollation, std::string & sError ) = 0;
	virtual int GetItemSize () const = 0;
	virtual void SetItemsCount ( int iSize ) = 0;

	virtual void SetAttr ( uint32_t tRowID, int64_t tAttr ) = 0;
	virtual void SetAttr ( uint32_t tRowID, const uint8_t * pData, int iLength ) = 0;
	virtual void SetAttr ( uint32_t tRowID, const int64_t * pData, int iLength ) = 0;

	virtual void Flush () = 0;
	virtual void Done() = 0;

	virtual SIWriter_i * GetWriter ( std::string & sError ) = 0;
};

template<typename VALUE>
struct RawValue_T
{
	VALUE m_tValue = 0;
	uint32_t m_tRowid = 0;

	RawValue_T () = default;
	RawValue_T ( VALUE tVal, uint32_t tRowid )
		: m_tValue ( tVal )
		, m_tRowid ( tRowid )
	{}
};

template<typename VALUE>
bool RawValueCmp ( const VALUE & tA, const VALUE & tB )
{
	return ( tA.m_tValue==tB.m_tValue ? tA.m_tRowid<tB.m_tRowid : tA.m_tValue<tB.m_tValue );
}

template<>
bool RawValueCmp< RawValue_T<float> > ( const RawValue_T<float> & tA, const RawValue_T<float> & tB )
{
	return ( FloatEqual ( tA.m_tValue, tB.m_tValue ) ? tA.m_tRowid<tB.m_tRowid : tA.m_tValue<tB.m_tValue );
}

template<typename VALUE>
struct RawWriter_T : public RawWriter_i
{
	typedef RawValue_T<VALUE> RawValue_t;
	std::vector< RawValue_t > m_dRows; // value, rowid

	FileWriter_c m_tFile;
	std::vector<uint64_t> m_dOffset;
	uint64_t m_iFileSize = 0;
	AttrType_e m_eAttrType = AttrType_e::NONE;
	StrHash_fn m_fnHash { nullptr };

	RawWriter_T() = default;

	bool Setup ( const char * sFile, int iAttr, AttrType_e eAttrType, Collation_e eCollation, std::string & sError ) final
	{
		m_eAttrType = eAttrType;
		m_fnHash = GetHashFn ( eCollation );
		std::string sFilename = FormatStr ( "%s.%d.tmp", sFile, iAttr );
		return m_tFile.Open ( sFilename, true, true, false, sError );
	}

	int GetItemSize () const final { return sizeof ( m_dRows[0] ); }
	void SetItemsCount ( int iSize ) final { m_dRows.reserve ( iSize ); }

	void Flush () final
	{
		size_t iBytesLen = sizeof( m_dRows[0] ) * m_dRows.size();
		if ( !iBytesLen )
			return;

		std::sort ( m_dRows.begin(), m_dRows.end(), RawValueCmp<RawValue_t> );

		m_dOffset.emplace_back ( m_tFile.GetPos() );
		m_tFile.Write ( (const uint8_t *)m_dRows.data(), iBytesLen );

		m_dRows.resize ( 0 ); 
	}

	void Done() final
	{
		Flush();
		m_iFileSize = m_tFile.GetPos();
		m_tFile.Close();
		VectorReset ( m_dRows );
	}

	void	SetAttr ( uint32_t tRowID, int64_t tAttr ) final;
	void	SetAttr ( uint32_t tRowID, const uint8_t * pData, int iLength ) final;
	void	SetAttr ( uint32_t tRowID, const int64_t * pData, int iLength ) final;

	SIWriter_i * GetWriter ( std::string & sError ) final;
};

static const std::string g_sCompressionUINT32 = "simdfastpfor128";
static const std::string g_sCompressionUINT64 = "fastpfor128";

class Builder_c final : public Builder_i
{
public:
	bool	Setup ( const std::vector<SourceAttrTrait_t> & dSrcAttrs, int iMemoryLimit, Collation_e eCollation, const char * sFile, std::string & sError );

	void	SetRowID ( uint32_t tRowID ) final;
	void	SetAttr ( int iAttr, int64_t tAttr ) final;
	void	SetAttr ( int iAttr, const uint8_t * pData, int iLength ) final;
	void	SetAttr ( int iAttr, const int64_t * pData, int iLength ) final;
	bool	Done ( std::string & sError ) final;

private:
	std::string m_sFile;
	uint32_t m_tRowID = 0;
	uint32_t m_iMaxRows = 0;

	std::vector<std::shared_ptr<RawWriter_i>>	m_dRawWriter;
	std::vector<std::shared_ptr<SIWriter_i>>	m_dCidWriter;

	std::vector<ColumnInfo_t>					m_dAttrs;
	Collation_e									m_eCollation;

	void Flush();
	bool WriteMeta ( const std::string & sPgmName, const std::string & sBlocksName, const std::vector<uint64_t> & dBlocksOffStart, const std::vector<uint64_t> & dBlocksCount, uint64_t uMetaOff, std::string & sError ) const;
};


bool Builder_c::Setup ( const std::vector<SourceAttrTrait_t> & dSrcAttrs, int iMemoryLimit, Collation_e eCollation, const char * sFile, std::string & sError )
{
	m_sFile = sFile;
	m_dRawWriter.resize ( dSrcAttrs.size() );
	if ( dSrcAttrs.size() )
		m_dRawWriter.resize ( dSrcAttrs.back().m_iAttr + 1 );
	m_eCollation = eCollation;

	for ( const SourceAttrTrait_t & tSrcAttr : dSrcAttrs )
	{
		std::shared_ptr<RawWriter_i> pWriter;
		switch ( tSrcAttr.m_eType )
		{
		case AttrType_e::UINT32:
		case AttrType_e::TIMESTAMP:
		case AttrType_e::UINT32SET:
			pWriter.reset ( new RawWriter_T<uint32_t> () );
			break;

		case AttrType_e::FLOAT:
			pWriter.reset ( new RawWriter_T<float>() );
			break;

		case AttrType_e::STRING:
			pWriter.reset ( new RawWriter_T<uint64_t>() );
			break;

		case AttrType_e::INT64:
		case AttrType_e::INT64SET:
			pWriter.reset ( new RawWriter_T<int64_t>() );
			break;

		default:
			break;
		}

		if ( pWriter )
		{
			if ( !pWriter->Setup ( sFile, tSrcAttr.m_iAttr, tSrcAttr.m_eType, eCollation, sError ) )
				return false;

			m_dRawWriter[tSrcAttr.m_iAttr] = pWriter;
			ColumnInfo_t tInfo;
			tInfo.m_eType = tSrcAttr.m_eType;
			tInfo.m_iSrcAttr = tSrcAttr.m_iAttr;
			tInfo.m_iAttr = (int)m_dAttrs.size();
			tInfo.m_sName = tSrcAttr.m_sName;
			m_dAttrs.push_back ( tInfo );
		}
	}

	int iRowSize = 0;
	for ( const auto & pWriter : m_dRawWriter )
	{
		if ( pWriter )
			iRowSize += pWriter->GetItemSize();
	}

	m_iMaxRows = std::max ( 1000, iMemoryLimit / 3 / iRowSize );

	for ( auto & pWriter : m_dRawWriter )
	{
		if ( pWriter )
			pWriter->SetItemsCount ( m_iMaxRows );
	}


	return true;
}

void Builder_c::SetRowID ( uint32_t tRowID )
{
	m_tRowID = tRowID;

	if ( ( m_tRowID % m_iMaxRows )==0 )
		Flush();
}

void Builder_c::SetAttr ( int iAttr, int64_t tAttr )
{
	if ( iAttr<m_dRawWriter.size() && m_dRawWriter[iAttr] )
		m_dRawWriter[iAttr]->SetAttr ( m_tRowID, tAttr );
}

void Builder_c::SetAttr ( int iAttr, const uint8_t * pData, int iLength )
{
	if ( iAttr<m_dRawWriter.size() && m_dRawWriter[iAttr] )
		m_dRawWriter[iAttr]->SetAttr ( m_tRowID, pData, iLength );
}

void Builder_c::SetAttr ( int iAttr, const int64_t * pData, int iLength )
{
	if ( iAttr<m_dRawWriter.size() && m_dRawWriter[iAttr] )
		m_dRawWriter[iAttr]->SetAttr ( m_tRowID, pData, iLength );
}

bool Builder_c::Done ( std::string & sError )
{
	// flush tail attributes
	for ( auto & pWriter : m_dRawWriter )
	{
		if ( pWriter )
			pWriter->Done();
	}

	// create Secondary Index writers
	for ( auto & pWriter : m_dRawWriter )
	{
		if ( pWriter )
		{
			SIWriter_i * pCidx = pWriter->GetWriter ( sError );
			if ( !pCidx )
				return false;
			m_dCidWriter.emplace_back ( pCidx );
		}
	}

	// free memory
	VectorReset ( m_dRawWriter );

	// pack values into lists
	FileWriter_c tDstFile;
	if ( !tDstFile.Open ( m_sFile, true, true, false, sError ) )
		return false;

	std::string sBlocksName = m_sFile + ".tmp.meta";
	FileWriter_c tTmpBlocks;
	if ( !tTmpBlocks.Open ( sBlocksName, true, true, true, sError ) )
		return false;

	std::string sPgmName = m_sFile + ".tmp.pgm";
	FileWriter_c tTmpPgm;
	if ( !tTmpPgm.Open ( sPgmName, true, true, true, sError ) )
		return false;

	std::string sPgmValuesName = m_sFile + ".tmp.pgmvalues";

	// reserve space at main file for meta
	tDstFile.Write_uint32 ( LIB_VERSION ); // version of library that builds the index
	tDstFile.Write_uint64 ( 0 ); // offset to meta itself

	std::vector<uint64_t> dBlocksOffStart ( m_dCidWriter.size() );
	std::vector<uint64_t> dBlocksCount ( m_dCidWriter.size() );

	// process raw attributes into column index
	for ( size_t iWriter=0; iWriter<m_dCidWriter.size(); iWriter++ )
	{
		dBlocksOffStart[iWriter] = tTmpBlocks.GetPos();

		auto & pWriter = m_dCidWriter[iWriter];
		if ( !pWriter->Process ( tDstFile, tTmpBlocks, sPgmValuesName, sError ) )
			return false;

		// temp meta
		WriteVectorLen ( pWriter->m_dPGM, tTmpPgm );

		// clean up used memory
		m_dCidWriter[iWriter] = nullptr;
	}

	int64_t iLastBlock = tTmpBlocks.GetPos();
	for ( size_t iBlock=1; iBlock<dBlocksCount.size(); iBlock++ )
	{
		dBlocksCount[iBlock-1] = ( dBlocksOffStart[iBlock] - dBlocksOffStart[iBlock-1] ) / sizeof ( dBlocksOffStart[iBlock] );
	}
	dBlocksCount.back() = ( iLastBlock - dBlocksOffStart.back() ) / sizeof ( dBlocksOffStart.back() );

	// meta
	uint64_t uMetaOff = tDstFile.GetPos();
	tDstFile.Close();
	// close temp writers
	tTmpBlocks.Close();
	tTmpPgm.Close();

	// write header and meta
	ComputeDeltas ( dBlocksOffStart.data(), (int)dBlocksOffStart.size(), true );
	return WriteMeta ( sPgmName, sBlocksName, dBlocksOffStart, dBlocksCount, uMetaOff, sError );
}

bool Builder_c::WriteMeta ( const std::string & sPgmName, const std::string & sBlocksName, const std::vector<uint64_t> & dBlocksOffStart, const std::vector<uint64_t> & dBlocksCount, uint64_t uMetaOff, std::string & sError ) const
{
	uint64_t uNextMeta = 0;

	{
		FileWriter_c tDstFile;
		if ( !tDstFile.Open ( m_sFile, false, false, false, sError ) )
			return false;

		// put meta offset to the begining
		tDstFile.Seek ( sizeof(uint32_t) );
		tDstFile.Write_uint64 ( uMetaOff );

		// append meta after blocks
		tDstFile.Seek ( uMetaOff );

		tDstFile.Write_uint64 ( uNextMeta ); // link to next meta
		tDstFile.Write_uint32 ( (int)m_dAttrs.size() );
		
		BitVec_t dAttrsEnabled ( m_dAttrs.size() );
		std::fill ( dAttrsEnabled.m_dData.begin(), dAttrsEnabled.m_dData.end(), 0xffffffff );
		WriteVector ( dAttrsEnabled.m_dData, tDstFile );

		tDstFile.Write_string ( g_sCompressionUINT32 );
		tDstFile.Write_string ( g_sCompressionUINT64 );
		tDstFile.Write_uint32 ( (uint32_t)m_eCollation );
		tDstFile.Write_uint32 ( VALUES_PER_BLOCK );
		
		// write schema
		for ( const auto & tInfo : m_dAttrs )
		{
			tDstFile.Write_string ( tInfo.m_sName );
			tDstFile.Pack_uint32 ( tInfo.m_iSrcAttr );
			tDstFile.Pack_uint32 ( tInfo.m_iAttr );
			tDstFile.Pack_uint32 ( (int)tInfo.m_eType );
		}

		WriteVectorPacked ( dBlocksOffStart, tDstFile );
		WriteVectorPacked ( dBlocksCount, tDstFile );
	}

	// append pgm indexes after meta
	if ( !CopySingleFile ( sPgmName, m_sFile, sError, 0 ) )
		return false;
	
	// append offsets to blocks
	if ( !CopySingleFile ( sBlocksName, m_sFile, sError, 0 ) )
		return false;

	return true;
}

void Builder_c::Flush()
{
	for ( auto & pWriter : m_dRawWriter )
	{
		if ( pWriter )
			pWriter->Flush();
	}
}

// raw int writer
template<>
inline void RawWriter_T<uint32_t>::SetAttr ( uint32_t tRowID, int64_t tAttr )
{
	m_dRows.emplace_back ( RawValue_T<uint32_t> { (uint32_t)tAttr, tRowID } );
}

template<>
inline void RawWriter_T<uint32_t>::SetAttr ( uint32_t tRowID, const uint8_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending string to int packer" );
}

template<>
inline void RawWriter_T<int64_t>::SetAttr ( uint32_t tRowID, const uint8_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending string to int packer" );
}

// raw int64 writer
template<>
inline void RawWriter_T<int64_t>::SetAttr ( uint32_t tRowID, int64_t tAttr )
{
	m_dRows.emplace_back ( RawValue_T<int64_t> { (int64_t)tAttr, tRowID } );
}

// raw string writer
template<>
inline void RawWriter_T<uint64_t>::SetAttr ( uint32_t tRowID, const uint8_t * pData, int iLength )
{
	m_dRows.emplace_back ( RawValue_T<uint64_t> { m_fnHash ( pData, iLength ), tRowID } );
}

template<>
inline void RawWriter_T<uint64_t>::SetAttr ( uint32_t tRowID, int64_t tAttr )
{
	assert ( 0 && "INTERNAL ERROR: sending string to int packer" );
}

template<>
inline void RawWriter_T<uint64_t>::SetAttr ( uint32_t tRowID, const int64_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending string to int packer" );
}

// raw MVA32 writer
template<>
inline void RawWriter_T<uint32_t>::SetAttr ( uint32_t tRowID, const int64_t * pData, int iLength )
{
	for ( int i=0; i<iLength; i++ )
		m_dRows.emplace_back ( RawValue_T<uint32_t> { (uint32_t)pData[i], tRowID } );
}

// raw MVA64 writer
template<>
inline void RawWriter_T<int64_t>::SetAttr ( uint32_t tRowID, const int64_t * pData, int iLength )
{
	for ( int i=0; i<iLength; i++ )
		m_dRows.emplace_back ( RawValue_T<int64_t> { pData[i], tRowID } );
}

// raw float writer
template<>
inline void RawWriter_T<float>::SetAttr ( uint32_t tRowID, int64_t tAttr )
{
	m_dRows.emplace_back ( RawValue_T<float> { UintToFloat ( tAttr ), tRowID } );
}

template<>
inline void RawWriter_T<float>::SetAttr ( uint32_t tRowID, const uint8_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending string to float packer" );
}

template<>
inline void RawWriter_T<float>::SetAttr ( uint32_t tRowID, const int64_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending MVA to float packer" );
}

template<typename SRC_VALUE, typename DST_VALUE>
struct SIWriter_T : public SIWriter_i
{
	std::string m_sSrcName;
	uint64_t m_iFileSize = 0;

	SIWriter_T () = default;
	virtual ~SIWriter_T() = default;

	bool Setup ( const std::string & sSrcFile, uint64_t iFileSize, std::vector<uint64_t> & dOffset, std::string & sError ) final;
	bool Process ( FileWriter_c & tDstFile, FileWriter_c & tTmpBlocksOff, const std::string & sPgmValuesName, std::string & sError ) final;
};

template<typename VALUE>
SIWriter_i * RawWriter_T<VALUE>::GetWriter ( std::string & sError )
{
	std::unique_ptr<SIWriter_i> pWriter { nullptr };
	switch ( m_eAttrType )
	{
	case AttrType_e::FLOAT:
		pWriter.reset ( new SIWriter_T<float, uint32_t>() );
		break;

	case AttrType_e::STRING:
		pWriter.reset ( new SIWriter_T<uint64_t, uint64_t>() );
		break;

	case AttrType_e::INT64:
	case AttrType_e::INT64SET:
		pWriter.reset ( new SIWriter_T<int64_t, uint64_t>() );
		break;

	default:
		pWriter.reset ( new SIWriter_T<uint32_t, uint32_t> () );
		break;
	}

	if ( !pWriter->Setup ( m_tFile.GetFilename(), m_iFileSize, m_dOffset, sError ) )
		return nullptr;

	return pWriter.release();
}

template<typename SRC_VALUE, typename DST_VALUE>
bool SIWriter_T<SRC_VALUE, DST_VALUE>::Setup ( const std::string & sSrcName, uint64_t iFileSize, std::vector<uint64_t> & dOffset, std::string & sError )
{
	m_dOffset = std::move ( dOffset );
	m_sSrcName = sSrcName;
	m_iFileSize = iFileSize;

	return true;
}

template<typename VALUE>
struct BinValue_T : public RawValue_T<VALUE>
{
	columnar::FileReader_c * m_pReader = nullptr;
	int64_t m_iBinEnd = 0;

	bool Read ()
	{
		if ( m_pReader->GetPos()>=m_iBinEnd )
			return false;
		
		m_pReader->Read ( (uint8_t *)( this ), sizeof ( RawValue_T<VALUE> ) );
		return true;
	}
};

template<typename VALUE>
struct PQGreater
{
	bool operator() ( const BinValue_T<VALUE> & tA, const BinValue_T<VALUE> & tB ) const;
};

template<typename VALUE>
bool PQGreater<VALUE>::operator() ( const BinValue_T<VALUE> & tA, const BinValue_T<VALUE> & tB ) const
{
	return ( tA.m_tValue==tB.m_tValue ? tA.m_tRowid>tB.m_tRowid : tA.m_tValue>tB.m_tValue );
}

template<>
bool PQGreater<float>::operator() ( const BinValue_T<float> & tA, const BinValue_T<float> & tB ) const
{
	return ( FloatEqual ( tA.m_tValue, tB.m_tValue ) ? tA.m_tRowid>tB.m_tRowid : tA.m_tValue>tB.m_tValue );
}

RawValue_T<uint32_t> Convert ( const BinValue_T<uint32_t> & tSrc )
{
	return tSrc;
}

RawValue_T<uint32_t> Convert ( const BinValue_T<float> & tSrc )
{
	RawValue_T<uint32_t> tRes;
	tRes.m_tValue = FloatToUint ( tSrc.m_tValue );
	tRes.m_tRowid = tSrc.m_tRowid;
	return tRes;
}

RawValue_T<uint64_t> Convert ( const BinValue_T<int64_t> & tSrc )
{
	RawValue_T<uint64_t> tRes;
	tRes.m_tValue = (uint64_t)tSrc.m_tValue;
	tRes.m_tRowid = tSrc.m_tRowid;
	return tRes;
}

RawValue_T<uint64_t> Convert ( const BinValue_T<uint64_t> & tSrc )
{
	return tSrc;
}

template<typename VEC>
void EncodeRowsBlock ( VEC & dSrcRows, uint32_t iOff, uint32_t iCount, IntCodec_i * pCodec, std::vector<uint32_t> & dBufRows, MemWriter_c & tWriter )
{
	Span_T<uint32_t> dRows ( dSrcRows.data() + iOff, iCount );
	if ( FastPForLib::needPaddingTo128Bits( dRows.begin() ) )
	{
		memmove ( dSrcRows.data(), dSrcRows.data() + iOff, sizeof(dSrcRows[0]) * iCount );
		dRows = Span_T<uint32_t> ( dSrcRows.data(), iCount );
	}

	dBufRows.resize ( 0 );
	const auto tMin = dRows.front();
	const auto tMax = dRows.back();

	ComputeDeltas ( dRows.data(), (int)dRows.size(), true );
	pCodec->Encode ( dRows, dBufRows );

	// block meta: 3 DWORD
	// [min-row-id, row-id-delta, block-size]
	// [packed block]
	tWriter.Pack_uint32 ( tMin );
	tWriter.Pack_uint32 ( tMax - tMin );
	WriteVectorLen32 ( dBufRows, tWriter );
}

template<typename VEC>
void EncodeBlock ( VEC & dSrc, IntCodec_i * pCodec, std::vector<uint32_t> & dBuf, FileWriter_c & tWriter )
{
	dBuf.resize ( 0 );

	ComputeDeltas ( dSrc.data(), (int)dSrc.size(), true );
	pCodec->Encode ( dSrc, dBuf );

	WriteVectorLen32 ( dBuf, tWriter );
}

template<typename VEC>
void EncodeBlockWoDelta ( VEC & dSrc, IntCodec_i * pCodec, std::vector<uint32_t> & dBuf, FileWriter_c & tWriter )
{
	dBuf.resize ( 0 );

	pCodec->Encode ( dSrc, dBuf );
	WriteVectorLen32 ( dBuf, tWriter );
}

template<typename VALUE>
void WriteRawValues ( const std::vector<VALUE> & dSrc, FileWriter_c & tWriter ) = delete;

template<>
void WriteRawValues<> ( const std::vector<uint32_t> & dSrc, FileWriter_c & tWriter )
{
	for ( uint32_t uVal : dSrc )
		tWriter.Write_uint32 ( uVal );
}

template<>
void WriteRawValues<> ( const std::vector<uint64_t> & dSrc, FileWriter_c & tWriter )
{
	for ( uint64_t uVal : dSrc )
		tWriter.Write_uint64 ( uVal );
}

template<typename VALUE, bool FLOAT_VALUE>
struct RowWriter_t
{
	std::vector<VALUE> m_dValues;
	std::vector<uint32_t> m_dTypes;
	std::vector<uint32_t> m_dRowStart;
	std::vector<uint32_t> m_dRows;

	std::vector<uint32_t> m_dBufTmp;
	std::vector<uint8_t> m_dRowsPacked;
	VALUE m_tLastValue { 0 };

	std::unique_ptr<IntCodec_i>	m_pCodec { nullptr };

	FileWriter_c * m_pBlocksOff { nullptr };
	FileWriter_c * m_pPGMVals { nullptr };

	RowWriter_t()
	{
		m_dValues.reserve ( VALUES_PER_BLOCK );
		m_dRowStart.reserve ( VALUES_PER_BLOCK );
		m_dRows.reserve ( VALUES_PER_BLOCK * 16 );

		m_dBufTmp.reserve ( VALUES_PER_BLOCK );
		m_dRowsPacked.reserve ( VALUES_PER_BLOCK * 16 );

		m_pCodec.reset ( CreateIntCodec ( g_sCompressionUINT32, g_sCompressionUINT64 ) );

	}

	void FlushValue ( FileWriter_c & tWriter )
	{
		if ( m_dValues.size()<VALUES_PER_BLOCK )
			return;

		FlushBlock ( tWriter );
	}

	void FlushBlock ( FileWriter_c & tWriter )
	{
		assert ( m_dValues.size()==m_dRowStart.size() );
		if ( !m_dValues.size() )
			return;

		const uint32_t iValues = (uint32_t)m_dValues.size();
		// FIXME!!! set flags: IsValsAsc \ IsValsDesc and CalcDelta with these flags or skip delta encoding
		//assert ( std::is_sorted ( m_dValues.begin(), m_dValues.end() ) );

		// replace with FLAGS
		bool bLenDelta = true;

		// FIXME!!! pack per block meta

		// pack rows
		MemWriter_c tBlockWriter ( m_dRowsPacked );
		m_dTypes.resize ( iValues );
		for ( size_t iItem=0; iItem<iValues; iItem++)
		{
			uint32_t uSrcRowsStart = m_dRowStart[iItem];
			size_t uSrcRowsCount = (  iItem+1<m_dRowStart.size() ? m_dRowStart[iItem+1] - uSrcRowsStart : m_dRows.size() - uSrcRowsStart );

			if ( uSrcRowsCount==1 )
			{
				m_dTypes[iItem] = (uint32_t)Packing_e::ROW;
				m_dRowStart[iItem] = m_dRows[uSrcRowsStart];
				bLenDelta = false;

			} else if ( uSrcRowsCount<=VALUES_PER_BLOCK )
			{
				m_dTypes[iItem] = (uint32_t)Packing_e::ROW_BLOCK;
				m_dRowStart[iItem] = (uint32_t)tBlockWriter.GetPos();

				EncodeRowsBlock ( m_dRows, uSrcRowsStart, (int)uSrcRowsCount, m_pCodec.get(), m_dBufTmp, tBlockWriter );
			} else
			{
				m_dTypes[iItem] = (uint32_t)Packing_e::ROW_BLOCKS_LIST;
				m_dRowStart[iItem] = (uint32_t)tBlockWriter.GetPos();

				int iBlocks = (int)( ( uSrcRowsCount + VALUES_PER_BLOCK - 1 ) / VALUES_PER_BLOCK );
				// block meta: 1 DWORD
				// blocks count
				tBlockWriter.Pack_uint32 ( iBlocks );

				for ( int iBlock=0; iBlock<iBlocks; iBlock++ )
				{
					uint32_t uSrcStart = uSrcRowsStart + iBlock * VALUES_PER_BLOCK;
					uint32_t uSrcCount = VALUES_PER_BLOCK;
					if ( iBlock==iBlocks-1 )
						uSrcCount = (uint32_t)( uSrcRowsCount - ( iBlock * VALUES_PER_BLOCK ) );

					EncodeRowsBlock ( m_dRows, uSrcStart, uSrcCount, m_pCodec.get(), m_dBufTmp, tBlockWriter );
				}
			}
		}

		// write offset to block into temporary file
		m_pBlocksOff->Write_uint64 ( tWriter.GetPos() );
		// write values for PGM builder
		WriteRawValues ( m_dValues, *m_pPGMVals );

		// write into file
		EncodeBlock ( m_dValues, m_pCodec.get(), m_dBufTmp, tWriter );
		EncodeBlockWoDelta ( m_dTypes, m_pCodec.get(), m_dBufTmp, tWriter );
		tWriter.Write_uint8 ( (uint8_t)bLenDelta );
		if ( bLenDelta )
		{
			EncodeBlock ( m_dRowStart, m_pCodec.get(), m_dBufTmp, tWriter );
		} else
		{
			EncodeBlockWoDelta ( m_dRowStart, m_pCodec.get(), m_dBufTmp, tWriter );
		}
		WriteVector ( m_dRowsPacked, tWriter );

		m_dValues.resize ( 0 );
		m_dTypes.resize ( 0 );
		m_dRowStart.resize ( 0 );
		m_dRows.resize ( 0 );
		m_dRowsPacked.resize ( 0 );
	}

	void Done ( FileWriter_c & tWriter )
	{
		FlushBlock ( tWriter );
	}

	void AddValue ( const RawValue_T<VALUE> & tBin )
	{
		m_dRowStart.push_back ( (uint32_t)m_dRows.size() );

		m_dValues.push_back ( tBin.m_tValue );
		m_dRows.push_back ( tBin.m_tRowid );
		m_tLastValue = tBin.m_tValue;
	}

	void NextValue ( const RawValue_T<VALUE> & tBin, FileWriter_c & m_tDstFile )
	{
		// collect row-list
		// or flush and store new value
		if ( FLOAT_VALUE && ( FloatEqual ( UintToFloat ( m_tLastValue ), UintToFloat ( tBin.m_tValue ) ) ) )
		{
			m_dRows.push_back ( tBin.m_tRowid );
		}
		else if ( !FLOAT_VALUE && m_tLastValue==tBin.m_tValue ) 
		{
			m_dRows.push_back ( tBin.m_tRowid );
		} else
		{
			FlushValue ( m_tDstFile );
			AddValue ( tBin );
		}
	}
};

template<typename SRC_VALUE, typename DST_VALUE>
bool SIWriter_T<SRC_VALUE, DST_VALUE>::Process ( FileWriter_c & tDstFile, FileWriter_c & tTmpBlocksOff, const std::string & sPgmValuesName, std::string & sError )
{
#if BUILD_PRINT_VALUES
	std::cout << m_sSrcName << std::endl;
#endif

	FileWriter_c tTmpValsPGM;
	if ( !tTmpValsPGM.Open ( sPgmValuesName, true, false, true, sError ) )
		return false;

	std::priority_queue< BinValue_T<SRC_VALUE>, std::vector < BinValue_T<SRC_VALUE> >, PQGreater<SRC_VALUE> > dBins;

	std::vector<std::unique_ptr< columnar::FileReader_c > > dSrcFile ( m_dOffset.size() );
	for ( int iReader=0; iReader<m_dOffset.size(); iReader++ )
	{
		columnar::FileReader_c * pReader = new columnar::FileReader_c();
		dSrcFile[iReader].reset ( pReader );

		if ( !pReader->Open ( m_sSrcName, sError ) )
			return false;

		pReader->Seek ( m_dOffset[iReader] );
		// set file chunk end
		int64_t iBinEnd = 0;
		if ( iReader<m_dOffset.size()-1 )
			iBinEnd = m_dOffset[iReader+1];
		else
			iBinEnd = m_iFileSize;

		BinValue_T<SRC_VALUE> tBin;
		tBin.m_pReader = pReader;
		tBin.m_iBinEnd = iBinEnd;
		tBin.Read();

		dBins.push ( tBin );
	}

	RowWriter_t<DST_VALUE, std::is_floating_point<SRC_VALUE>::value > tWriter;
	tWriter.m_pBlocksOff = &tTmpBlocksOff;
	tWriter.m_pPGMVals = &tTmpValsPGM;

	// initial fill
	if ( dBins.size() )
	{
		BinValue_T<SRC_VALUE> tBin = dBins.top();
		dBins.pop();
		tWriter.AddValue ( Convert ( tBin ) );
		if ( tBin.Read() )
			dBins.push ( tBin );
	}

	while ( !dBins.empty() )
	{
		BinValue_T<SRC_VALUE> tBin = dBins.top();
		dBins.pop();

		tWriter.NextValue ( Convert ( tBin ), tDstFile );

		if ( tBin.Read() )
			dBins.push ( tBin );
	}

	tWriter.Done ( tDstFile );

	dSrcFile.clear(); // to free up memory for PGM build phase
	::unlink ( m_sSrcName.c_str() );

	tTmpValsPGM.Close();
	MappedBuffer_T<SRC_VALUE> tMappedPGM;
	if ( !tMappedPGM.Open ( sPgmValuesName, sError ) )
		return false;

	assert ( std::is_sorted ( tMappedPGM.begin(), tMappedPGM.end() ) );
	PGM_T<SRC_VALUE> tInv ( tMappedPGM.begin(), tMappedPGM.end() );
	tInv.Save ( m_dPGM );

#if BUILD_PRINT_VALUES
	for ( int i=0; i<tPGMVals.size(); i++ )
	{
		ApproxPos_t tRes = tInv.Search ( tPGMVals[i] );
		if ( tRes.m_iLo/3!=tRes.m_iHi/3 )
			std::cout << "val[" << i << "] " << (uint32_t)tPGMVals[i] << ", lo " << tRes.m_iLo/3 << ", hi " << tRes.m_iHi/3 << ", pos " << tRes.m_iPos/3 << " " << std::endl;
	}
#endif

	return true;
}

static std::array<StrHash_fn, (size_t)Collation_e::TOTAL> g_dCollations;
uint64_t g_uHashSeed = 0;

StrHash_fn GetHashFn ( Collation_e eCollation )
{
	return g_dCollations[(size_t)eCollation];
}

} // namespace SI

void CollationInit ( const std::array<SI::StrHash_fn, (size_t)SI::Collation_e::TOTAL> & dCollations )
{
	SI::g_dCollations = dCollations;
}

SI::Builder_i * CreateBuilder ( const std::vector<SI::SourceAttrTrait_t> & dSrcAttrs, int iMemoryLimit, SI::Collation_e eCollation, const char * sFile, std::string & sError )
{
	std::unique_ptr<SI::Builder_c> pBuilder ( new SI::Builder_c );

	if ( !pBuilder->Setup ( dSrcAttrs, iMemoryLimit, eCollation, sFile, sError ) )
		return nullptr;

	return pBuilder.release();
}

int GetSecondaryLibVersion()
{
	return SI::LIB_VERSION;
}

extern const char * LIB_VERSION;
const char * GetSecondaryLibVersionStr()
{
	return LIB_VERSION;
}

int GetSecondaryStorageVersion()
{
	return SI::STORAGE_VERSION;
}
