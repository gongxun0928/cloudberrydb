//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2012 EMC Corp.
//
//	@filename:
//		CPhysicalSequenceProject.cpp
//
//	@doc:
//		Implementation of physical sequence project operator
//---------------------------------------------------------------------------

#include "gpopt/operators/CPhysicalSequenceProject.h"

#include "gpos/base.h"

#include "gpopt/base/CDistributionSpecAny.h"
#include "gpopt/base/CDistributionSpecHashed.h"
#include "gpopt/base/CDistributionSpecReplicated.h"
#include "gpopt/base/CDistributionSpecSingleton.h"
#include "gpopt/base/COptCtxt.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/base/CWindowFrame.h"
#include "gpopt/cost/ICostModel.h"
#include "gpopt/operators/CExpressionHandle.h"
#include "gpopt/operators/CLogicalSequenceProject.h"
#include "gpopt/operators/CScalarIdent.h"

using namespace gpopt;

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::CPhysicalSequenceProject
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CPhysicalSequenceProject::CPhysicalSequenceProject(CMemoryPool *mp,
												   ESPType sptype,
												   CDistributionSpec *pds,
												   COrderSpecArray *pdrgpos,
												   CWindowFrameArray *pdrgpwf)
	: CPhysical(mp),
	  m_sptype(sptype),
	  m_pds(pds),
	  m_pdrgpos(pdrgpos),
	  m_pdrgpwf(pdrgpwf),
	  m_pos(nullptr),
	  m_pcrsRequiredLocal(nullptr)
{
	GPOS_ASSERT(nullptr != pds);
	GPOS_ASSERT(nullptr != pdrgpos);
	GPOS_ASSERT(nullptr != pdrgpwf);
	GPOS_ASSERT(CDistributionSpec::EdtHashed == pds->Edt() ||
				CDistributionSpec::EdtSingleton == pds->Edt());
	// we don't create LogicalSequenceProject with equivalent hashed distribution specs at this time
	if (CDistributionSpec::EdtHashed == pds->Edt())
	{
		GPOS_ASSERT(nullptr ==
					CDistributionSpecHashed::PdsConvert(pds)->PdshashedEquiv());
	}
	CreateOrderSpec(mp);
	ComputeRequiredLocalColumns(mp);
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::CreateOrderSpec
//
//	@doc:
//		Create local order spec that we request relational child to satisfy
//
//---------------------------------------------------------------------------
void
CPhysicalSequenceProject::CreateOrderSpec(CMemoryPool *mp)
{
	GPOS_ASSERT(nullptr == m_pos);
	GPOS_ASSERT(nullptr != m_pds);
	GPOS_ASSERT(nullptr != m_pdrgpos);

	m_pos = GPOS_NEW(mp) COrderSpec(mp);

	// add partition by keys to order spec
	if (CDistributionSpec::EdtHashed == m_pds->Edt())
	{
		CDistributionSpecHashed *pdshashed =
			CDistributionSpecHashed::PdsConvert(m_pds);

		const CExpressionArray *pdrgpexpr = pdshashed->Pdrgpexpr();
		const ULONG size = pdrgpexpr->Size();
		for (ULONG ul = 0; ul < size; ul++)
		{
			CExpression *pexpr = (*pdrgpexpr)[ul];
			// we assume partition-by keys are always scalar idents
			CScalarIdent *popScId = CScalarIdent::PopConvert(pexpr->Pop());
			const CColRef *colref = popScId->Pcr();

			gpmd::IMDId *mdid =
				colref->RetrieveType()->GetMdidForCmpType(IMDType::EcmptL);
			mdid->AddRef();

			m_pos->Append(mdid, colref, COrderSpec::EntLast);
		}
	}

	if (0 == m_pdrgpos->Size())
	{
		return;
	}

	COrderSpec *posFirst = (*m_pdrgpos)[0];
#ifdef GPOS_DEBUG
	const ULONG length = m_pdrgpos->Size();
	for (ULONG ul = 1; ul < length; ul++)
	{
		COrderSpec *posCurrent = (*m_pdrgpos)[ul];
		GPOS_ASSERT(posFirst->FSatisfies(posCurrent) &&
					"first order spec must satisfy all other order specs");
	}
#endif	// GPOS_DEBUG

	// we assume here that the first order spec in the children array satisfies all other
	// order specs in the array, this happens as part of the initial normalization
	// so we need to add columns only from the first order spec
	const ULONG size = posFirst->UlSortColumns();
	for (ULONG ul = 0; ul < size; ul++)
	{
		const CColRef *colref = posFirst->Pcr(ul);
		gpmd::IMDId *mdid = posFirst->GetMdIdSortOp(ul);
		mdid->AddRef();
		COrderSpec::ENullTreatment ent = posFirst->Ent(ul);
		m_pos->Append(mdid, colref, ent);
	}
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::ComputeRequiredLocalColumns
//
//	@doc:
//		Compute local required columns
//
//---------------------------------------------------------------------------
void
CPhysicalSequenceProject::ComputeRequiredLocalColumns(CMemoryPool *mp)
{
	GPOS_ASSERT(nullptr != m_pos);
	GPOS_ASSERT(nullptr != m_pds);
	GPOS_ASSERT(nullptr != m_pdrgpos);
	GPOS_ASSERT(nullptr != m_pdrgpwf);
	GPOS_ASSERT(nullptr == m_pcrsRequiredLocal);

	m_pcrsRequiredLocal = m_pos->PcrsUsed(mp);
	if (CDistributionSpec::EdtHashed == m_pds->Edt())
	{
		CColRefSet *pcrsHashed =
			CDistributionSpecHashed::PdsConvert(m_pds)->PcrsUsed(mp);
		m_pcrsRequiredLocal->Include(pcrsHashed);
		pcrsHashed->Release();
	}

	// add the columns used in the window frames
	const ULONG size = m_pdrgpwf->Size();
	for (ULONG ul = 0; ul < size; ul++)
	{
		CWindowFrame *pwf = (*m_pdrgpwf)[ul];
		if (nullptr != pwf->PexprLeading())
		{
			m_pcrsRequiredLocal->Union(
				pwf->PexprLeading()->DeriveUsedColumns());
		}
		if (nullptr != pwf->PexprTrailing())
		{
			m_pcrsRequiredLocal->Union(
				pwf->PexprTrailing()->DeriveUsedColumns());
		}
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::~CPhysicalSequenceProject
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CPhysicalSequenceProject::~CPhysicalSequenceProject()
{
	m_pds->Release();
	m_pdrgpos->Release();
	m_pdrgpwf->Release();
	m_pos->Release();
	m_pcrsRequiredLocal->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::Matches
//
//	@doc:
//		Match operators
//
//---------------------------------------------------------------------------
BOOL
CPhysicalSequenceProject::Matches(COperator *pop) const
{
	GPOS_ASSERT(nullptr != pop);
	if (Eopid() == pop->Eopid())
	{
		CPhysicalSequenceProject *popPhysicalSequenceProject =
			CPhysicalSequenceProject::PopConvert(pop);
		return m_sptype == popPhysicalSequenceProject->Pspt() &&
			   m_pds->Matches(popPhysicalSequenceProject->Pds()) &&
			   CWindowFrame::Equals(m_pdrgpwf,
									popPhysicalSequenceProject->Pdrgpwf()) &&
			   COrderSpec::Equals(m_pdrgpos,
								  popPhysicalSequenceProject->Pdrgpos());
	}

	return false;
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::HashValue
//
//	@doc:
//		Hashing function
//
//---------------------------------------------------------------------------
ULONG
CPhysicalSequenceProject::HashValue() const
{
	ULONG ulHash = 0;
	ulHash = gpos::CombineHashes(ulHash, m_pds->HashValue());
	ulHash = gpos::CombineHashes(
		ulHash, CWindowFrame::HashValue(m_pdrgpwf, 3 /*ulMaxSize*/));
	ulHash = gpos::CombineHashes(
		ulHash, COrderSpec::HashValue(m_pdrgpos, 3 /*ulMaxSize*/));

	return ulHash;
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::PcrsRequired
//
//	@doc:
//		Compute required columns of the n-th child;
//		we only compute required columns for the relational child;
//
//---------------------------------------------------------------------------
CColRefSet *
CPhysicalSequenceProject::PcrsRequired(CMemoryPool *mp,
									   CExpressionHandle &exprhdl,
									   CColRefSet *pcrsRequired,
									   ULONG child_index,
									   CDrvdPropArray *,  // pdrgpdpCtxt
									   ULONG			  // ulOptReq
)
{
	GPOS_ASSERT(
		0 == child_index &&
		"Required properties can only be computed on the relational child");

	CColRefSet *pcrs = GPOS_NEW(mp) CColRefSet(mp, *m_pcrsRequiredLocal);
	pcrs->Union(pcrsRequired);

	CColRefSet *pcrsOutput =
		PcrsChildReqd(mp, exprhdl, pcrs, child_index, 1 /*ulScalarIndex*/);
	pcrs->Release();

	return pcrsOutput;
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::PosRequired
//
//	@doc:
//		Compute required sort order of the n-th child
//
//---------------------------------------------------------------------------
COrderSpec *
CPhysicalSequenceProject::PosRequired(CMemoryPool *,		// mp
									  CExpressionHandle &,	// exprhdl
									  COrderSpec *,			// posRequired
									  ULONG
#ifdef GPOS_DEBUG
										  child_index
#endif	// GPOS_DEBUG
									  ,
									  CDrvdPropArray *,	 // pdrgpdpCtxt
									  ULONG				 // ulOptReq
) const
{
	GPOS_ASSERT(0 == child_index);

	m_pos->AddRef();

	return m_pos;
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::PdsRequired
//
//	@doc:
//		Compute required distribution of the n-th child
//
//---------------------------------------------------------------------------
CDistributionSpec *
CPhysicalSequenceProject::PdsRequired(CMemoryPool *mp,
									  CExpressionHandle &exprhdl,
									  CDistributionSpec *pdsRequired,
									  ULONG child_index,
									  CDrvdPropArray *,	 // pdrgpdpCtxt
									  ULONG				 // ulOptReq
) const
{
	GPOS_ASSERT(0 == child_index);

	// if expression has to execute on a single host then we need a gather
	if (exprhdl.NeedsSingletonExecution())
	{
		return PdsRequireSingleton(mp, exprhdl, pdsRequired, child_index);
	}

	if (m_sptype == COperator::EsptypeLocal)
	{
		return GPOS_NEW(mp) CDistributionSpecAny(this->Eopid());
	}

	// if there are outer references, then we need a broadcast (or a gather)
	if (exprhdl.HasOuterRefs())
	{
		if (CDistributionSpec::EdtSingleton == pdsRequired->Edt() ||
			CDistributionSpec::EdtStrictReplicated == pdsRequired->Edt())
		{
			return PdsPassThru(mp, exprhdl, pdsRequired, child_index);
		}

		return GPOS_NEW(mp)
			CDistributionSpecReplicated(CDistributionSpec::EdtStrictReplicated);
	}

	// if the window operator has a partition by clause, then always
	// request hashed distribution on the partition column
	if (CDistributionSpec::EdtHashed == m_pds->Edt())
	{
		m_pds->AddRef();
		return m_pds;
	}

	return GPOS_NEW(mp) CDistributionSpecSingleton();
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::PrsRequired
//
//	@doc:
//		Compute required rewindability of the n-th child
//
//---------------------------------------------------------------------------
CRewindabilitySpec *
CPhysicalSequenceProject::PrsRequired(CMemoryPool *mp,
									  CExpressionHandle &exprhdl,
									  CRewindabilitySpec *prsRequired,
									  ULONG child_index,
									  CDrvdPropArray *,	 // pdrgpdpCtxt
									  ULONG				 // ulOptReq
) const
{
	GPOS_ASSERT(0 == child_index);

	return PrsPassThru(mp, exprhdl, prsRequired, child_index);
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::PcteRequired
//
//	@doc:
//		Compute required CTE map of the n-th child
//
//---------------------------------------------------------------------------
CCTEReq *
CPhysicalSequenceProject::PcteRequired(CMemoryPool *,		 //mp,
									   CExpressionHandle &,	 //exprhdl,
									   CCTEReq *pcter,
									   ULONG
#ifdef GPOS_DEBUG
										   child_index
#endif
									   ,
									   CDrvdPropArray *,  //pdrgpdpCtxt,
									   ULONG			  //ulOptReq
) const
{
	GPOS_ASSERT(0 == child_index);
	return PcterPushThru(pcter);
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::FProvidesReqdCols
//
//	@doc:
//		Check if required columns are included in output columns
//
//---------------------------------------------------------------------------
BOOL
CPhysicalSequenceProject::FProvidesReqdCols(CExpressionHandle &exprhdl,
											CColRefSet *pcrsRequired,
											ULONG  // ulOptReq
) const
{
	GPOS_ASSERT(nullptr != pcrsRequired);
	GPOS_ASSERT(2 == exprhdl.Arity());

	CColRefSet *pcrs = GPOS_NEW(m_mp) CColRefSet(m_mp);
	// include defined columns by scalar project list
	pcrs->Union(exprhdl.DeriveDefinedColumns(1));

	// include output columns of the relational child
	pcrs->Union(exprhdl.DeriveOutputColumns(0 /*child_index*/));

	BOOL fProvidesCols = pcrs->ContainsAll(pcrsRequired);
	pcrs->Release();

	return fProvidesCols;
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::PosDerive
//
//	@doc:
//		Derive sort order
//
//---------------------------------------------------------------------------
COrderSpec *
CPhysicalSequenceProject::PosDerive(CMemoryPool *,	// mp
									CExpressionHandle &exprhdl) const
{
	return PosDerivePassThruOuter(exprhdl);
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::PdsDerive
//
//	@doc:
//		Derive distribution
//
//---------------------------------------------------------------------------
CDistributionSpec *
CPhysicalSequenceProject::PdsDerive(CMemoryPool *mp,
									CExpressionHandle &exprhdl) const
{
	CDistributionSpec *pds = exprhdl.Pdpplan(0 /*child_index*/)->Pds();
	if (CDistributionSpec::EdtStrictReplicated == pds->Edt())
	{
		// Sequence project (i.e. window functions) cannot guarantee replicated
		// data if their windowing clause combined with the function's input column
		// is under specified.
		// If the child was replicated, we can no longer guarantee that
		// property. Therefore we must now dervive tainted replicated.
		return GPOS_NEW(mp) CDistributionSpecReplicated(
			CDistributionSpec::EdtTaintedReplicated);
	}
	else
	{
		return PdsDerivePassThruOuter(exprhdl);
	}
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::PrsDerive
//
//	@doc:
//		Derive rewindability
//
//---------------------------------------------------------------------------
CRewindabilitySpec *
CPhysicalSequenceProject::PrsDerive(CMemoryPool *mp,
									CExpressionHandle &exprhdl) const
{
	return PrsDerivePassThruOuter(mp, exprhdl);
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::EpetOrder
//
//	@doc:
//		Return the enforcing type for order property based on this operator
//
//---------------------------------------------------------------------------
CEnfdProp::EPropEnforcingType
CPhysicalSequenceProject::EpetOrder(CExpressionHandle &exprhdl,
									const CEnfdOrder *peo) const
{
	GPOS_ASSERT(nullptr != peo);
	GPOS_ASSERT(!peo->PosRequired()->IsEmpty());

	COrderSpec *pos = CDrvdPropPlan::Pdpplan(exprhdl.Pdp())->Pos();
	if (peo->FCompatible(pos))
	{
		return CEnfdProp::EpetUnnecessary;
	}

	return CEnfdProp::EpetRequired;
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::EpetRewindability
//
//	@doc:
//		Return the enforcing type for rewindability property based on this operator
//
//---------------------------------------------------------------------------
CEnfdProp::EPropEnforcingType
CPhysicalSequenceProject::EpetRewindability(CExpressionHandle &exprhdl,
											const CEnfdRewindability *per) const
{
	CRewindabilitySpec *prs = CDrvdPropPlan::Pdpplan(exprhdl.Pdp())->Prs();
	if (per->FCompatible(prs))
	{
		// required distribution is already provided
		return CEnfdProp::EpetUnnecessary;
	}

	// rewindability is enforced on operator's output
	return CEnfdProp::EpetRequired;
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalSequenceProject::OsPrint
//
//	@doc:
//		debug print
//
//---------------------------------------------------------------------------
IOstream &
CPhysicalSequenceProject::OsPrint(IOstream &os) const
{
	os << SzId() << " (";
	CLogicalSequenceProject::OsPrintWindowType(os, m_sptype);
	os << ") (";
	(void) m_pds->OsPrint(os);
	os << ", ";
	(void) COrderSpec::OsPrint(os, m_pdrgpos);
	os << ", ";
	(void) CWindowFrame::OsPrint(os, m_pdrgpwf);

	return os << ")";
}


// EOF
