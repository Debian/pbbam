// Copyright (c) 2014-2015, Pacific Biosciences of California, Inc.
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted (subject to the limitations in the
// disclaimer below) provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
//  * Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//
//  * Neither the name of Pacific Biosciences nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
// GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY PACIFIC
// BIOSCIENCES AND ITS CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
// OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL PACIFIC BIOSCIENCES OR ITS
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
// USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
// OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.

// Author: Derek Barnett

#include "pbbam/BamRecord.h"
#include "pbbam/virtual/VirtualRegionTypeMap.h"
#include "AssertUtils.h"
#include "MemoryUtils.h"
#include "SequenceUtils.h"
#include <htslib/sam.h>

#include <iostream>
#include <stdexcept>

using namespace PacBio;
using namespace PacBio::BAM;
using namespace std;

namespace PacBio {
namespace BAM {
namespace internal {

// BAM record tag names
static const string tagName_readAccuracy            = "rq";
static const string tagName_holeNumber              = "zm";
static const string tagName_numPasses               = "np";
static const string tagName_contextFlags            = "cx";
static const string tagName_snr                     = "sn";
static const string tagName_deletionQV              = "dq";
static const string tagName_deletionTag             = "dt";
static const string tagName_insertionQV             = "iq";
static const string tagName_ipd                     = "ip";
static const string tagName_mergeQV                 = "mq";
static const string tagName_pulseWidth              = "pw";
static const string tagName_readGroup               = "RG";
static const string tagName_queryStart              = "qs";
static const string tagName_queryEnd                = "qe";
static const string tagName_substitutionQV          = "sq";
static const string tagName_substitutionTag         = "st";
static const string tagName_pkmean                  = "pa";
static const string tagName_pkmid                   = "pm";
static const string tagName_pre_pulse_frames        = "pd";
static const string tagName_pulse_call_width        = "px";
static const string tagName_labelQV                 = "lq";
static const string tagName_labelTag                = "lt";
static const string tagName_alternative_labelQV     = "aq";
static const string tagName_alternative_labelTag    = "at";
static const string tagName_pulse_call              = "pc";
static const string tagName_scrap_type              = "sc";
static const string tagName_barcodes                = "bc";

// faux (helper) tag names
static const string tagName_QUAL = "QUAL";
static const string tagName_SEQ  = "SEQ";

// record type names
static const string recordTypeName_Polymerase = "POLYMERASE";
static const string recordTypeName_HqRegion   = "HQREGION";
static const string recordTypeName_Subread    = "SUBREAD";
static const string recordTypeName_CCS        = "CCS";
static const string recordTypeName_Scrap      = "SCRAP";
static const string recordTypeName_Unknown    = "UNKNOWN";

static
int32_t HoleNumberFromName(const string& fullName)
{
    const vector<string> mainTokens = std::move(Split(fullName, '/'));
    if (mainTokens.size() != 3)
        throw std::runtime_error("malformed record name");
    return stoi(mainTokens.at(1));
}

static
Position QueryEndFromName(const string& fullName)
{
    const vector<string> mainTokens = std::move(Split(fullName, '/'));
    if (mainTokens.size() != 3)
        throw std::runtime_error("malformed record name");
    const vector<string> queryTokens = std::move(Split(mainTokens.at(2), '_'));
    if (queryTokens.size() != 2)
        throw std::runtime_error("malformed record name");
    return stoi(queryTokens.at(1));
}

static
Position QueryStartFromName(const string& fullName)
{
    const vector<string> mainTokens = std::move(Split(fullName, '/'));
    if (mainTokens.size() != 3)
        throw std::runtime_error("malformed record name");
    const vector<string> queryTokens = std::move(Split(mainTokens.at(2), '_'));
    if (queryTokens.size() != 2)
        throw std::runtime_error("malformed record name");
    return stoi(queryTokens.at(0));
}

static
BamRecordImpl* CreateOrEdit(const string& tagName,
                            const Tag& value,
                            BamRecordImpl* impl)
{
    if (impl->HasTag(tagName))
        impl->EditTag(tagName, value);
    else
        impl->AddTag(tagName, value);
    return impl;
}

static
int32_t AlignedEndOffset(const Cigar& cigar,
                         const int seqLength)
{
    int32_t endOffset = seqLength;

    if (!cigar.empty()) {
        Cigar::const_reverse_iterator cigarIter = cigar.crbegin();
        Cigar::const_reverse_iterator cigarEnd  = cigar.crend();
        for (; cigarIter != cigarEnd; ++cigarIter) {
            const CigarOperation& op = (*cigarIter);
            if (op.Type() == CigarOperationType::HARD_CLIP) {
                if (endOffset != 0 && endOffset != seqLength)
                    return -1;
            }
            else if (op.Type() == CigarOperationType::SOFT_CLIP)
                endOffset -= op.Length();
            else
                break;
        }
    }

    if (endOffset == 0)
        endOffset = seqLength;
    return endOffset;
}

static
int32_t AlignedStartOffset(const Cigar& cigar,
                           const int seqLength)
{
    int32_t startOffset = 0;

    if (!cigar.empty()) {
        Cigar::const_iterator cigarIter = cigar.cbegin();
        Cigar::const_iterator cigarEnd  = cigar.cend();
        for (; cigarIter != cigarEnd; ++cigarIter) {
            const CigarOperation& op = (*cigarIter);
            if (op.Type() == CigarOperationType::HARD_CLIP) {
                if (startOffset != 0 && startOffset != seqLength)
                    return -1;
            }
            else if (op.Type() == CigarOperationType::SOFT_CLIP)
                startOffset += op.Length();
            else
                break;
        }
    }
    return startOffset;
}

template<typename T>
T Clip(const T& input,
       const size_t pos,
       const size_t len)
{
    return T(input.cbegin() + pos,
             input.cbegin() + pos + len);
}

static
void MaybeClipAndGapifyBases(const BamRecordImpl& impl,
                             const bool aligned,
                             const bool exciseSoftClips,
                             string& seq)
{
    if (impl.IsMapped() && (aligned || exciseSoftClips)) {

        size_t seqIndex = 0;
        const Cigar& cigar = impl.CigarData();
        Cigar::const_iterator cigarIter = cigar.cbegin();
        Cigar::const_iterator cigarEnd  = cigar.cend();
        for (; cigarIter != cigarEnd; ++cigarIter) {
            const CigarOperation& op = (*cigarIter);
            const CigarOperationType& type = op.Type();

            // do nothing for hard clips
            if (type != CigarOperationType::HARD_CLIP) {
                const size_t opLength = op.Length();

                // maybe remove soft clips
                if (type == CigarOperationType::SOFT_CLIP && exciseSoftClips)
                    seq.erase(seqIndex, opLength);

                // for non-clipping operations
                else {

                    // maybe add gaps/padding
                    if (aligned) {
                        if (type == CigarOperationType::DELETION) {
                            seq.reserve(seq.size() + opLength);
                            seq.insert(seqIndex, opLength, '-');
                        }
                        else if (type == CigarOperationType::PADDING) {
                            seq.reserve(seq.size() + opLength);
                            seq.insert(seqIndex, opLength, '*');
                        }
                    }

                    // update index
                    seqIndex += opLength;
                }
            }
        }
    }
}

static
void MaybeClipAndGapifyFrames(const BamRecordImpl& impl,
                              const bool aligned,
                              const bool exciseSoftClips,
                              Frames& frames)
{
    if (impl.IsMapped() && (aligned || exciseSoftClips)) {

        vector<uint16_t> data = std::move(frames.Data()); // we're going to put it back
        size_t frameIndex = 0;
        const Cigar& cigar = impl.CigarData();
        Cigar::const_iterator cigarIter = cigar.cbegin();
        Cigar::const_iterator cigarEnd  = cigar.cend();
        for (; cigarIter != cigarEnd; ++cigarIter) {
            const CigarOperation& op = (*cigarIter);
            const CigarOperationType& type = op.Type();

            // do nothing for hard clips
            if (type != CigarOperationType::HARD_CLIP) {
                const size_t opLength = op.Length();

                // maybe remove soft clips
                if (type == CigarOperationType::SOFT_CLIP && exciseSoftClips)
                    data.erase(data.begin() + frameIndex, data.begin() + frameIndex + opLength);

                // for non-clipping operations
                else {

                    // maybe add gaps/padding
                    if (aligned) {
                        if (type == CigarOperationType::DELETION || type == CigarOperationType::PADDING) {
                            data.reserve(data.size() + opLength);
                            data.insert(data.begin() + frameIndex, opLength, 0);
                        }
                    }

                    // update index
                    frameIndex += opLength;
                }
            }
        }
        frames.Data(data);
    }
}

static
void MaybeClipAndGapifyQualities(const BamRecordImpl& impl,
                                 const bool aligned,
                                 const bool exciseSoftClips,
                                 QualityValues& qualities)
{
    if (impl.IsMapped() && (aligned || exciseSoftClips)) {

        size_t qualIndex = 0;
        const Cigar& cigar = impl.CigarData();
        Cigar::const_iterator cigarIter = cigar.cbegin();
        Cigar::const_iterator cigarEnd  = cigar.cend();
        for (; cigarIter != cigarEnd; ++cigarIter) {

            const CigarOperation& op = (*cigarIter);
            const CigarOperationType& type = op.Type();

            // do nothing for hard clips
            if (type != CigarOperationType::HARD_CLIP) {
                const size_t opLength = op.Length();

                // maybe remove soft clips
                if (type == CigarOperationType::SOFT_CLIP && exciseSoftClips)
                    qualities.erase(qualities.begin() + qualIndex, qualities.begin() + qualIndex + opLength);

                // for non-clipping operations
                else {

                    // maybe add gaps/padding
                    if (aligned) {
                        if (type == CigarOperationType::DELETION || type == CigarOperationType::PADDING) {
                            qualities.reserve(qualities.size() + opLength);
                            qualities.insert(qualities.begin() + qualIndex, opLength, QualityValue(0));
                        }
                    }

                    // update index
                    qualIndex += opLength;
                }
            }
        }
    }
}

static inline
void MaybeReverseFrames(const bool isReverseStrand,
                        const Orientation orientation,
                        std::vector<uint16_t>* data)
{
    const bool shouldReverse = isReverseStrand && orientation == Orientation::GENOMIC;
    if (shouldReverse)
        std::reverse(data->begin(), data->end());
}

static inline
void MaybeReverseFrames(const bool isReverseStrand,
                        const Orientation orientation,
                        Frames& frames)
{
    const bool shouldReverse = isReverseStrand && orientation == Orientation::GENOMIC;
    if (shouldReverse)
        std::reverse(frames.begin(), frames.end());
}

static inline
void MaybeReverseQuals(const bool isBamQual,
                       const bool isReverseStrand,
                       const Orientation orientation,
                       QualityValues& quals)
{
    const bool shouldReverse = (isBamQual ? isReverseStrand && orientation == Orientation::NATIVE
                                          : isReverseStrand && orientation == Orientation::GENOMIC);
    if (shouldReverse)
        std::reverse(quals.begin(), quals.end());
}

static inline
void MaybeReverseComplementSeq(const bool isPulse,
                               const bool isBamSeq,
                               const bool isReverseStrand,
                               const Orientation orientation,
                               string& seq)
{
    const bool shouldReverse = (isBamSeq ? isReverseStrand && orientation == Orientation::NATIVE
                                         : isReverseStrand && orientation == Orientation::GENOMIC);
    if (shouldReverse)
    {
        if (isPulse)
            internal::ReverseComplementCaseSens(seq);
        else
            internal::ReverseComplement(seq);
    }
}

static
RecordType NameToType(const string& name)
{
    if (name == recordTypeName_Subread)
        return RecordType::SUBREAD;
    if (name == recordTypeName_Polymerase)
        return RecordType::POLYMERASE;
    if (name == recordTypeName_HqRegion)
        return RecordType::HQREGION;
    if (name == recordTypeName_CCS)
        return RecordType::CCS;
    if (name == recordTypeName_Scrap)
        return RecordType::SCRAP;
    return RecordType::UNKNOWN;
}


} // namespace internal
} // namespace BAM
} // namespace PacBio

const float BamRecord::photonFactor = 10.0;

BamRecord::BamRecord(void)
    : alignedStart_(PacBio::BAM::UnmappedPosition)
    , alignedEnd_(PacBio::BAM::UnmappedPosition)
{ }

BamRecord::BamRecord(const BamHeader& header)
    : header_(header)
    , alignedStart_(PacBio::BAM::UnmappedPosition)
    , alignedEnd_(PacBio::BAM::UnmappedPosition)
{ }

BamRecord::BamRecord(const BamRecordImpl& impl)
    : impl_(impl)
    , alignedStart_(PacBio::BAM::UnmappedPosition)
    , alignedEnd_(PacBio::BAM::UnmappedPosition)
{ }

BamRecord::BamRecord(BamRecordImpl&& impl)
    : impl_(std::move(impl))
    , alignedStart_(PacBio::BAM::UnmappedPosition)
    , alignedEnd_(PacBio::BAM::UnmappedPosition)
{ }

BamRecord::BamRecord(const BamRecord& other)
    : impl_(other.impl_)
    , header_(other.header_)
    , alignedStart_(other.alignedStart_)
    , alignedEnd_(other.alignedEnd_)
{ }

BamRecord::BamRecord(BamRecord&& other)
    : impl_(std::move(other.impl_))
    , header_(std::move(other.header_))
    , alignedStart_(std::move(other.alignedStart_))
    , alignedEnd_(std::move(other.alignedEnd_))
{ }

BamRecord& BamRecord::operator=(const BamRecord& other)
{
    impl_ = other.impl_;
    header_ = other.header_;
    alignedStart_ = other.alignedStart_;
    alignedEnd_ = other.alignedEnd_;
    return *this;
}

BamRecord& BamRecord::operator=(BamRecord&& other)
{
    impl_ = std::move(other.impl_);
    header_ = std::move(other.header_);
    alignedStart_ = std::move(other.alignedStart_);
    alignedEnd_ = std::move(other.alignedEnd_);
    return *this;
}

BamRecord::~BamRecord(void) { }

Position BamRecord::AlignedEnd(void) const
{
    if (alignedEnd_ == PacBio::BAM::UnmappedPosition)
        CalculateAlignedPositions();
    return alignedEnd_;
}

Position BamRecord::AlignedStart(void) const
{
    if (alignedStart_ == PacBio::BAM::UnmappedPosition)
        CalculateAlignedPositions();
    return alignedStart_;
}

Strand BamRecord::AlignedStrand(void) const
{ return impl_.IsReverseStrand() ? Strand::REVERSE : Strand::FORWARD; }

QualityValues BamRecord::AltLabelQV(Orientation orientation, bool aligned,
                                    bool exciseSoftClips) const
{
    return FetchQualities(internal::tagName_alternative_labelQV,
                          orientation,
                          aligned,
                          exciseSoftClips);
}

BamRecord& BamRecord::AltLabelQV(const QualityValues& altLabelQVs)
{
    internal::CreateOrEdit(internal::tagName_alternative_labelQV,
                           altLabelQVs.Fastq(), &impl_);
    return *this;
}

std::string BamRecord::AltLabelTag(Orientation orientation,
                                   bool aligned,
                                   bool exciseSoftClips) const
{
    return FetchBases(internal::tagName_alternative_labelTag,
                      orientation,
                      aligned,
                      exciseSoftClips);
}

BamRecord& BamRecord::AltLabelTag(const std::string& tags)
{
    internal::CreateOrEdit(internal::tagName_alternative_labelTag, tags, &impl_);
    return *this;
}

std::pair<int,int> BamRecord::Barcodes(void) const
{
    const Tag& bc = impl_.TagValue(internal::tagName_barcodes);
    if (bc.IsNull())
        return std::make_pair(-1, -1);

    if (!bc.IsUInt16Array())
        throw std::runtime_error("Barcode tag bc is not of type uint16_t array.");

    const auto bcArray = bc.ToUInt16Array();
    if (bcArray.size() != 2)
        throw std::runtime_error("Barcode array is not of size 2");

    return std::make_pair(bcArray[0], bcArray[1]);
}

void BamRecord::CalculateAlignedPositions(void) const
{
    // reset
    ResetCachedPositions();

    // skip if unmapped, or has no queryStart/End
    if (!impl_.IsMapped())
        return;
    const Position qStart = QueryStart();
    const Position qEnd   = QueryEnd();
    if (qStart == PacBio::BAM::UnmappedPosition || qEnd == PacBio::BAM::UnmappedPosition)
        return;

    // determine clipped end ranges
    const Cigar& cigar     = impl_.CigarData();
    const size_t seqLength = impl_.Sequence().size();
    const int32_t startOffset = internal::AlignedStartOffset(cigar, seqLength);
    const int32_t endOffset   = internal::AlignedEndOffset(cigar, seqLength);
    if (endOffset == -1 || startOffset == -1)
        return; // TODO: handle error more??

    // store aligned positions (polymerase read coordinates)
    if (impl_.IsReverseStrand()) {
        alignedStart_ = qStart + (seqLength - endOffset);
        alignedEnd_   = qEnd - startOffset;
    }
    else {
        alignedStart_ = qStart + startOffset;
        alignedEnd_   = qEnd - (seqLength - endOffset);
    }
}

Cigar BamRecord::CigarData(void) const
{ return impl_.CigarData(); }

BamRecord& BamRecord::Clip(const ClipType clipType,
                           const Position start,
                           const Position end)
{
    // skip if no clip requested
    if (clipType == ClipType::CLIP_NONE)
        return *this;
    const bool clipToQuery = (clipType == ClipType::CLIP_TO_QUERY);

    // cache original coords
    const Position origQStart = QueryStart();
    const Position origQEnd   = QueryEnd();
    const Position origAStart = AlignedStart();
    const Position origAEnd   = AlignedEnd();

    // only used on mapped records
    Position origTStart;
    Position origTEnd;
    bool isForwardStrand = (AlignedStrand() == Strand::FORWARD);

    // cache any add'l coords, skip out if clip not needed (or not possible)
    if (clipToQuery) {
        if (start <= origQStart && end >= origQEnd)
            return *this;
    } else {

        assert(clipType == ClipType::CLIP_TO_REFERENCE);
        if (!IsMapped())
            return *this;

        origTStart = ReferenceStart();
        origTEnd   = ReferenceEnd();
        if (start <= origTStart && end >= origTEnd)
            return *this;

        assert(origAStart >= origQStart);
        assert(origAEnd   <= origQEnd);
    }

    // determine new offsets into data
    size_t startOffset;
    size_t endOffset;

    if (clipToQuery) {
        startOffset = start - origQStart;
        endOffset   = origQEnd - end;
    } else {

        const size_t alignedStartOffset = (origAStart - origQStart);
        const size_t alignedEndOffset = (origQEnd - origAEnd);
        const size_t tStartDiff = start - origTStart;
        const size_t tEndDiff   = origTEnd - end;

        if (isForwardStrand) {
            startOffset = alignedStartOffset + tStartDiff;
            endOffset = alignedEndOffset + tEndDiff;
        } else {
            startOffset = alignedEndOffset + tStartDiff;
            endOffset = alignedStartOffset + tEndDiff;
        }
    }

    size_t queryPosRemovedFront = 0;
    size_t queryPosRemovedBack  = 0;
    size_t refPosRemovedFront   = 0;
    size_t refPosRemovedBack    = 0;

    // if mapped
    if (IsMapped()) {

        // update CIGAR - clip front ops, then clip back ops
        Cigar cigar = std::move(impl_.CigarData());
        size_t offsetRemaining = startOffset;
        while (offsetRemaining > 0 && !cigar.empty()) {
            CigarOperation& firstOp = cigar.front();
            const CigarOperationType firstOpType = firstOp.Type();
            const size_t firstOpLength = firstOp.Length();

            const bool shouldUpdateQueryPos = ((bam_cigar_type(static_cast<int>(firstOpType)) & 0x1) != 0);
            const bool shouldUpdateRefPos = ((bam_cigar_type(static_cast<int>(firstOpType)) & 0x2) != 0);

            if (firstOpLength <= offsetRemaining) {

                cigar.erase(cigar.begin());

                if (shouldUpdateQueryPos)
                    queryPosRemovedFront += firstOpLength;
                if (shouldUpdateRefPos)
                    refPosRemovedFront += firstOpLength;

                offsetRemaining -= firstOpLength;

            } else {

                firstOp.Length(firstOpLength - offsetRemaining);

                if (shouldUpdateQueryPos)
                    queryPosRemovedFront += offsetRemaining;
                if (shouldUpdateRefPos)
                    refPosRemovedFront += offsetRemaining;

                offsetRemaining = 0;
            }
        }

        offsetRemaining = endOffset;
        while (offsetRemaining > 0 && !cigar.empty()) {
            CigarOperation& lastOp = cigar.back();
            const CigarOperationType lastOpType = lastOp.Type();
            const size_t lastOpLength = lastOp.Length();

            const bool shouldUpdateQueryPos = ((bam_cigar_type(static_cast<int>(lastOpType)) & 0x1) != 0);
            const bool shouldUpdateRefPos = ((bam_cigar_type(static_cast<int>(lastOpType)) & 0x2) != 0);

            if (lastOpLength <= offsetRemaining) {
                cigar.pop_back();

                if (shouldUpdateQueryPos)
                    queryPosRemovedBack += lastOpLength;
                if (shouldUpdateRefPos)
                    refPosRemovedBack += lastOpLength;

                offsetRemaining -= lastOpLength;

            } else {
                lastOp.Length(lastOpLength - offsetRemaining);

                if (shouldUpdateQueryPos)
                    queryPosRemovedBack += offsetRemaining;
                if (shouldUpdateRefPos)
                    refPosRemovedBack += offsetRemaining;

                offsetRemaining = 0;
            }
        }
        impl_.CigarData(cigar);

        // update aligned reference position
        if (clipToQuery) {
            const Position origPosition = impl_.Position();
            impl_.Position(origPosition + refPosRemovedFront);
        } else {
            impl_.Position(start);
        }
    }

    const string origSequence = std::move(Sequence(Orientation::GENOMIC));
    const QualityValues origQualities = std::move(Qualities(Orientation::GENOMIC));

    size_t clipIndex;
    size_t clipLength;
    if (clipToQuery) {
        clipIndex = startOffset;
        clipLength = (end - start);
    } else {
        const size_t origSeqLength = origSequence.length();
        const size_t newSeqLength = (origSeqLength - queryPosRemovedBack) - queryPosRemovedFront;
        clipIndex = queryPosRemovedFront;
        clipLength = newSeqLength;
    }

    // clip seq, quals
    const string sequence = std::move(internal::Clip(origSequence, clipIndex, clipLength));
    const QualityValues qualities = std::move(internal::Clip(origQualities, clipIndex, clipLength));
    impl_.SetSequenceAndQualities(sequence, qualities.Fastq());

    // clip PacBio tags
    QualityValues altLabelQV = std::move(internal::Clip(AltLabelQV(Orientation::GENOMIC), clipIndex, clipLength));
    QualityValues labelQV = std::move(internal::Clip(LabelQV(Orientation::GENOMIC), clipIndex, clipLength));
    QualityValues deletionQV = std::move(internal::Clip(DeletionQV(Orientation::GENOMIC), clipIndex, clipLength));
    QualityValues insertionQV = std::move(internal::Clip(InsertionQV(Orientation::GENOMIC), clipIndex, clipLength));
    QualityValues mergeQV = std::move(internal::Clip(MergeQV(Orientation::GENOMIC), clipIndex, clipLength));
    QualityValues substitutionQV = std::move(internal::Clip(SubstitutionQV(Orientation::GENOMIC), clipIndex, clipLength));
    Frames ipd = std::move(internal::Clip(IPD(Orientation::GENOMIC).Data(), clipIndex, clipLength));
    Frames pulseWidth = std::move(internal::Clip(PulseWidth(Orientation::GENOMIC).Data(), clipIndex, clipLength));
    string deletionTag = std::move(internal::Clip(DeletionTag(Orientation::GENOMIC), clipIndex, clipLength));
    string substitutionTag = std::move(internal::Clip(SubstitutionTag(Orientation::GENOMIC), clipIndex, clipLength));
    string labelTag = std::move(internal::Clip(LabelTag(Orientation::GENOMIC), clipIndex, clipLength));
    string altLabelTag = std::move(internal::Clip(AltLabelTag(Orientation::GENOMIC), clipIndex, clipLength));
    string pulseCall = std::move(PulseCall(Orientation::GENOMIC));
    std::vector<float> pkmean = std::move(Pkmean(Orientation::GENOMIC));
    std::vector<float> pkmid = std::move(Pkmid(Orientation::GENOMIC));
    Frames prePulseFrames = std::move(PrePulseFrames(Orientation::GENOMIC).Data());
    Frames pulseCallWidth = std::move(PulseCallWidth(Orientation::GENOMIC).Data());

    // restore native orientation
    if (!isForwardStrand) {
        internal::Reverse(altLabelQV);
        internal::Reverse(labelQV);
        internal::Reverse(deletionQV);
        internal::Reverse(insertionQV);
        internal::Reverse(mergeQV);
        internal::Reverse(substitutionQV);
        internal::Reverse(ipd);
        internal::Reverse(pulseWidth);
        internal::ReverseComplement(deletionTag);
        internal::ReverseComplement(substitutionTag);
        internal::ReverseComplement(labelTag);
        internal::ReverseComplement(altLabelTag);
        internal::ReverseComplementCaseSens(pulseCall);
        internal::Reverse(pkmean);
        internal::Reverse(pkmid);
        internal::Reverse(prePulseFrames);
        internal::Reverse(pulseCallWidth);
    }

    // update BAM tags
    TagCollection tags = impl_.Tags();
    tags[internal::tagName_alternative_labelQV] = altLabelQV.Fastq();
    tags[internal::tagName_labelQV]             = labelQV.Fastq();
    tags[internal::tagName_deletionQV]          = deletionQV.Fastq();
    tags[internal::tagName_insertionQV]         = insertionQV.Fastq();
    tags[internal::tagName_mergeQV]             = mergeQV.Fastq();
    tags[internal::tagName_substitutionQV]      = substitutionQV.Fastq();
    tags[internal::tagName_ipd]                 = ipd.Data();
    tags[internal::tagName_pulseWidth]          = pulseWidth.Data();
    tags[internal::tagName_deletionTag]         = deletionTag;
    tags[internal::tagName_substitutionTag]     = substitutionTag;
    tags[internal::tagName_labelTag]            = labelTag;
    tags[internal::tagName_alternative_labelTag]= altLabelTag;
    tags[internal::tagName_pulse_call]          = pulseCall;
    tags[internal::tagName_pkmean]              = EncodePhotons(pkmean);
    tags[internal::tagName_pkmid]               = EncodePhotons(pkmid);
    tags[internal::tagName_pre_pulse_frames]    = prePulseFrames.Data();
    tags[internal::tagName_pulse_call_width]    = pulseCallWidth.Data();
    impl_.Tags(tags);

    // update query start/end
    if (clipToQuery) {
        internal::CreateOrEdit(internal::tagName_queryStart, start, &impl_);
        internal::CreateOrEdit(internal::tagName_queryEnd,   end,   &impl_);
    } else {
        if (isForwardStrand) {
            const Position qStart = origQStart + queryPosRemovedFront;
            const Position qEnd   = origQEnd   - queryPosRemovedBack;
            internal::CreateOrEdit(internal::tagName_queryStart, qStart, &impl_);
            internal::CreateOrEdit(internal::tagName_queryEnd,   qEnd,   &impl_);
        } else {
            const Position qStart = origQStart + queryPosRemovedBack;
            const Position qEnd   = origQEnd   - queryPosRemovedFront;
            internal::CreateOrEdit(internal::tagName_queryStart, qStart, &impl_);
            internal::CreateOrEdit(internal::tagName_queryEnd,   qEnd,   &impl_);
        }
    }

    // reset any cached aligned start/end
    ResetCachedPositions();
    return *this;
}

QualityValues BamRecord::DeletionQV(Orientation orientation,
                                    bool aligned,
                                    bool exciseSoftClips) const
{
    return FetchQualities(internal::tagName_deletionQV,
                          orientation,
                          aligned,
                          exciseSoftClips);
}

BamRecord& BamRecord::DeletionQV(const QualityValues& deletionQVs)
{
    internal::CreateOrEdit(internal::tagName_deletionQV, deletionQVs.Fastq(), &impl_);
    return *this;
}


string BamRecord::DeletionTag(Orientation orientation,
                              bool aligned,
                              bool exciseSoftClips) const
{
    return FetchBases(internal::tagName_deletionTag,
                      orientation,
                      aligned,
                      exciseSoftClips);
}

BamRecord& BamRecord::DeletionTag(const std::string& tags)
{
    internal::CreateOrEdit(internal::tagName_deletionTag, tags, &impl_);
    return *this;
}

std::vector<uint16_t>
BamRecord::EncodePhotons(const std::vector<float>& data)
{
    std::vector<uint16_t> encoded;
    encoded.reserve(data.size());
    for (const auto& d : data)
        encoded.emplace_back(d * photonFactor);
    return encoded;
}

string BamRecord::FetchBasesRaw(const string& tagName) const
{
    const Tag& seqTag = impl_.TagValue(tagName);
    string seq = seqTag.ToString();
    return seq;
}

string BamRecord::FetchBases(const string& tagName,
                             const Orientation orientation) const
{
    const bool isBamSeq = (tagName == internal::tagName_SEQ);
    const bool isPulse = (tagName == internal::tagName_pulse_call);
    string seq = FetchBasesRaw(tagName);

    // rev-comp
    internal::MaybeReverseComplementSeq(isPulse,
                                        isBamSeq,
                                        impl_.IsReverseStrand(),
                                        orientation,
                                        seq);
    return seq;
}

string BamRecord::FetchBases(const string& tagName,
                             const Orientation orientation,
                             const bool aligned,
                             const bool exciseSoftClips) const
{
    const bool isPulse = (tagName == internal::tagName_pulse_call);
    const bool isBamSeq = (tagName == internal::tagName_SEQ);

    // fetch SAM/BAM SEQ field
    if (isBamSeq) {
        string seq = std::move(impl_.Sequence());

        // clip / gapify
        internal::MaybeClipAndGapifyBases(impl_,
                                          aligned,
                                          exciseSoftClips,
                                          seq);
        // rev-comp
        internal::MaybeReverseComplementSeq(isPulse,
                                            isBamSeq,
                                            impl_.IsReverseStrand(),
                                            orientation,
                                            seq);
        return seq;
    }

    // other tags of 'bases' type
    else {

        string seq = FetchBasesRaw(tagName);

        // rev-comp
        internal::MaybeReverseComplementSeq(isPulse,
                                            isBamSeq,
                                            impl_.IsReverseStrand(),
                                            orientation,
                                            seq);
        // clip / gapify
        internal::MaybeClipAndGapifyBases(impl_,
                                          aligned,
                                          exciseSoftClips,
                                          seq);
        return seq;
    }
}

Frames BamRecord::FetchFramesRaw(const string& tagName) const
{
    Frames frames;
    const Tag& frameTag = impl_.TagValue(tagName);
    if (frameTag.IsNull())
        return frames;

    // lossy frame codes
    if (frameTag.IsUInt8Array()) {
        const vector<uint8_t> codes = std::move(frameTag.ToUInt8Array());
        frames = std::move(Frames::Decode(codes));
    }

    // lossless frame data
    else {
        assert(frameTag.IsUInt16Array());
        const vector<uint16_t> losslessFrames = std::move(frameTag.ToUInt16Array());
        frames.Data(std::move(losslessFrames));
    }

    return frames;
}

Frames BamRecord::FetchFrames(const string& tagName,
                              const Orientation orientation) const
{
    Frames frames = FetchFramesRaw(tagName);

    // reverse, if needed
    internal::MaybeReverseFrames(impl_.IsReverseStrand(),
                                 orientation,
                                 frames);

    return frames;
}

Frames BamRecord::FetchFrames(const string& tagName,
                              const Orientation orientation,
                              const bool aligned,
                              const bool exciseSoftClips) const
{
    Frames frames = FetchFramesRaw(tagName);

    // reverse, if needed
    internal::MaybeReverseFrames(impl_.IsReverseStrand(),
                                 orientation,
                                 frames);

    // clip / gapify
    internal::MaybeClipAndGapifyFrames(impl_,
                                      aligned,
                                      exciseSoftClips,
                                      frames);

    return frames;
}

vector<float> BamRecord::FetchPhotons(const string& tagName,
                                      const Orientation orientation) const
{
    const Tag& frameTag = impl_.TagValue(tagName);
    if (frameTag.IsNull())
        return vector<float>();

    if(!frameTag.IsUInt16Array())
        throw std::runtime_error("Photons are not a uint16_t array, tag " + tagName);
    vector<uint16_t> data = std::move(frameTag.ToUInt16Array());

    // reverse, if needed
    internal::MaybeReverseFrames(impl_.IsReverseStrand(),
                                 orientation,
                                 &data);

    vector<float> photons;
    photons.reserve(data.size());

    for (const auto& d : data)
        photons.emplace_back(d / photonFactor);

    return photons;
}

QualityValues BamRecord::FetchQualities(const string& tagName,
                                        const Orientation orientation,
                                        const bool aligned,
                                        const bool exciseSoftClips) const
{
    const bool isBamQual = (tagName == internal::tagName_QUAL);

    // fetch SAM/BAM QUAL field
    if (isBamQual) {

        // fetch data
        QualityValues quals = std::move(impl_.Qualities());

        // clip / gapify
        internal::MaybeClipAndGapifyQualities(impl_,
                                              aligned,
                                              exciseSoftClips,
                                              quals);

        // rev-comp
        internal::MaybeReverseQuals(isBamQual,
                                    impl_.IsReverseStrand(),
                                    orientation,
                                    quals);
        return quals;
    }

    // other tags of 'qualities' type
    else {

        // fetch data
        const Tag& qvsTag = impl_.TagValue(tagName);
        QualityValues quals = std::move(QualityValues::FromFastq(qvsTag.ToString()));

        // rev-comp
        internal::MaybeReverseQuals(isBamQual,
                                    impl_.IsReverseStrand(),
                                    orientation,
                                    quals);
        // clip / gapify
        internal::MaybeClipAndGapifyQualities(impl_,
                                              aligned,
                                              exciseSoftClips,
                                              quals);
        return quals;
    }
}

string BamRecord::FullName(void) const
{ return impl_.Name(); }

bool BamRecord::HasAltLabelQV(void) const
{ return impl_.HasTag(internal::tagName_alternative_labelQV); }

bool BamRecord::HasAltLabelTag(void) const
{ return impl_.HasTag(internal::tagName_alternative_labelTag); }

bool BamRecord::HasBarcodes(void) const
{ return impl_.HasTag(internal::tagName_barcodes); }

bool BamRecord::HasLabelQV(void) const
{ return impl_.HasTag(internal::tagName_labelQV); }

bool BamRecord::HasDeletionQV(void) const
{ return impl_.HasTag(internal::tagName_deletionQV); }

bool BamRecord::HasDeletionTag(void) const
{ return impl_.HasTag(internal::tagName_deletionTag); }

bool BamRecord::HasHoleNumber(void) const
{ return impl_.HasTag(internal::tagName_holeNumber)
          && !impl_.TagValue(internal::tagName_holeNumber).IsNull();
}

bool BamRecord::HasInsertionQV(void) const
{ return impl_.HasTag(internal::tagName_insertionQV); }

bool BamRecord::HasPreBaseFrames(void) const
{ return HasIPD(); }

bool BamRecord::HasIPD(void) const
{ return impl_.HasTag(internal::tagName_ipd); }

bool BamRecord::HasLabelTag(void) const
{ return impl_.HasTag(internal::tagName_labelTag); }

bool BamRecord::HasLocalContextFlags(void) const
{ return impl_.HasTag(internal::tagName_contextFlags); }

bool BamRecord::HasMergeQV(void) const
{ return impl_.HasTag(internal::tagName_mergeQV); }

bool BamRecord::HasPkmean(void) const
{ return impl_.HasTag(internal::tagName_pkmean); }

bool BamRecord::HasPkmid(void) const
{ return impl_.HasTag(internal::tagName_pkmid); }

bool BamRecord::HasPrePulseFrames(void) const
{ return impl_.HasTag(internal::tagName_pre_pulse_frames); }

bool BamRecord::HasPulseCall(void) const
{ return impl_.HasTag(internal::tagName_pulse_call)
          && !impl_.TagValue(internal::tagName_pulse_call).IsNull();
}

bool BamRecord::HasPulseCallWidth(void) const
{ return impl_.HasTag(internal::tagName_pulse_call_width); }

bool BamRecord::HasPulseWidth(void) const
{ return impl_.HasTag(internal::tagName_pulseWidth); }

bool BamRecord::HasQueryEnd(void) const
{ return impl_.HasTag(internal::tagName_queryEnd); }

bool BamRecord::HasQueryStart(void) const
{ return impl_.HasTag(internal::tagName_queryStart); }

bool BamRecord::HasReadAccuracy(void) const
{ return impl_.HasTag(internal::tagName_readAccuracy)
          && !impl_.TagValue(internal::tagName_readAccuracy).IsNull();
}

bool BamRecord::HasScrapType(void) const
{ return impl_.HasTag(internal::tagName_scrap_type)
          && !impl_.TagValue(internal::tagName_scrap_type).IsNull();
}

bool BamRecord::HasSignalToNoise(void) const
{ return impl_.HasTag(internal::tagName_snr); }

bool BamRecord::HasSubstitutionQV(void) const
{ return impl_.HasTag(internal::tagName_substitutionQV); }

bool BamRecord::HasSubstitutionTag(void) const
{ return impl_.HasTag(internal::tagName_substitutionTag); }

BamHeader BamRecord::Header(void) const
{ return header_; }

int32_t BamRecord::HoleNumber(void) const
{
    const Tag& holeNumber = impl_.TagValue(internal::tagName_holeNumber);
    if (!holeNumber.IsNull())
        return holeNumber.ToInt32();

    // missing zm tag - try to pull from name
    return internal::HoleNumberFromName(FullName());
}

BamRecord& BamRecord::HoleNumber(const int32_t holeNumber)
{
    internal::CreateOrEdit(internal::tagName_holeNumber,
                           holeNumber,
                           &impl_);
    return *this;
}

BamRecordImpl& BamRecord::Impl(void)
{ return impl_; }

const BamRecordImpl& BamRecord::Impl(void) const
{ return impl_; }

QualityValues BamRecord::InsertionQV(Orientation orientation,
                                     bool aligned,
                                     bool exciseSoftClips) const
{
    return FetchQualities(internal::tagName_insertionQV,
                          orientation,
                          aligned,
                          exciseSoftClips);
}

BamRecord& BamRecord::InsertionQV(const QualityValues& insertionQVs)
{
    internal::CreateOrEdit(internal::tagName_insertionQV, insertionQVs.Fastq(), &impl_);
    return *this;
}

Frames BamRecord::IPD(Orientation orientation,
                      bool aligned,
                      bool exciseSoftClips) const
{
    return FetchFrames(internal::tagName_ipd,
                       orientation,
                       aligned,
                       exciseSoftClips);
}

BamRecord& BamRecord::IPD(const Frames& frames,
                          const FrameEncodingType encoding)
{
    if (encoding == FrameEncodingType::LOSSY)
        internal::CreateOrEdit(internal::tagName_ipd, frames.Encode(), &impl_);
    else
        internal::CreateOrEdit(internal::tagName_ipd, frames.Data(), &impl_);
    return *this;
}

Frames BamRecord::PreBaseFrames(Orientation orientation, 
                                bool aligned,
                                bool exciseSoftClips) const
{ return IPD(orientation,aligned,exciseSoftClips); }

BamRecord& BamRecord::PreBaseFrames(const Frames& frames,
                                    const FrameEncodingType encoding)
{ return IPD(frames, encoding); }

Frames BamRecord::IPDRaw(Orientation orientation) const
{
    const auto tagName = internal::tagName_ipd;

    Frames frames;
    const Tag& frameTag = impl_.TagValue(tagName);
    if (frameTag.IsNull())
        return frames;

    // lossy frame codes
    if (frameTag.IsUInt8Array()) {
        const vector<uint8_t> codes = std::move(frameTag.ToUInt8Array());
        const vector<uint16_t> codes16(codes.begin(), codes.end());
        frames.Data(std::move(codes16));
    }

    // lossless frame data
    else {
        assert(frameTag.IsUInt16Array());
        const vector<uint16_t> losslessFrames = std::move(frameTag.ToUInt16Array());
        frames.Data(std::move(losslessFrames));
    }

    // reverse, if needed
    internal::MaybeReverseFrames(impl_.IsReverseStrand(),
                                 orientation,
                                 frames);

    return frames;
}

Frames BamRecord::PulseWidthRaw(Orientation orientation) const
{
    const auto tagName = internal::tagName_pulseWidth;

    Frames frames;
    const Tag& frameTag = impl_.TagValue(tagName);
    if (frameTag.IsNull())
        return frames;

    // lossy frame codes
    if (frameTag.IsUInt8Array()) {
        const vector<uint8_t> codes = std::move(frameTag.ToUInt8Array());
        const vector<uint16_t> codes16(codes.begin(), codes.end());
        frames.Data(std::move(codes16));
    }

    // lossless frame data
    else {
        assert(frameTag.IsUInt16Array());
        const vector<uint16_t> losslessFrames = std::move(frameTag.ToUInt16Array());
        frames.Data(std::move(losslessFrames));
    }

    // reverse, if needed
    internal::MaybeReverseFrames(impl_.IsReverseStrand(),
                                 orientation,
                                 frames);

    return frames;
}

bool BamRecord::IsMapped(void) const
{ return impl_.IsMapped(); }

QualityValues BamRecord::LabelQV(Orientation orientation, bool aligned,
                                 bool exciseSoftClips) const
{
    return FetchQualities(internal::tagName_labelQV,
                          orientation,
                          aligned,
                          exciseSoftClips);
}

BamRecord& BamRecord::LabelQV(const QualityValues& labelQVs)
{
    internal::CreateOrEdit(internal::tagName_labelQV, labelQVs.Fastq(), &impl_);
    return *this;
}

std::string BamRecord::LabelTag(Orientation orientation,
                                bool aligned,
                                bool exciseSoftClips) const
{
    return FetchBases(internal::tagName_labelTag,
                      orientation,
                      aligned,
                      exciseSoftClips);
}

BamRecord& BamRecord::LabelTag(const std::string& tags)
{
    internal::CreateOrEdit(internal::tagName_labelTag, tags, &impl_);
    return *this;
}

LocalContextFlags BamRecord::LocalContextFlags(void) const
{
    const Tag& cxTag = impl_.TagValue(internal::tagName_contextFlags);
    return static_cast<PacBio::BAM::LocalContextFlags>(cxTag.ToUInt8());
}

BamRecord& BamRecord::LocalContextFlags(const PacBio::BAM::LocalContextFlags flags)
{
    internal::CreateOrEdit(internal::tagName_contextFlags,
                           static_cast<uint8_t>(flags),
                           &impl_);
    return *this;
}

BamRecord& BamRecord::Map(const int32_t referenceId,
                          const Position refStart,
                          const Strand strand,
                          const Cigar& cigar,
                          const uint8_t mappingQuality)
{
    impl_.Position(refStart);
    impl_.ReferenceId(referenceId);
    impl_.CigarData(cigar);
    impl_.MapQuality(mappingQuality);
    impl_.SetMapped(true);

    if (strand == Strand::FORWARD)
        impl_.SetReverseStrand(false);

    else {
        assert(strand == Strand::REVERSE);
        impl_.SetReverseStrand(true);

        // switch seq & qual
        string sequence  = impl_.Sequence();
        QualityValues qualities = impl_.Qualities();

        internal::ReverseComplement(sequence);
        internal::Reverse(qualities);

        impl_.SetSequenceAndQualities(sequence, qualities.Fastq());
    }

    // reset any cached aligned start/end
    alignedStart_ = PacBio::BAM::UnmappedPosition;
    alignedEnd_ = PacBio::BAM::UnmappedPosition;

    return *this;
}

uint8_t BamRecord::MapQuality(void) const
{ return impl_.MapQuality(); }

QualityValues BamRecord::MergeQV(Orientation orientation,
                                 bool aligned,
                                 bool exciseSoftClips) const
{
    return FetchQualities(internal::tagName_mergeQV,
                          orientation,
                          aligned,
                          exciseSoftClips);
}

BamRecord& BamRecord::MergeQV(const QualityValues& mergeQVs)
{
    internal::CreateOrEdit(internal::tagName_mergeQV, mergeQVs.Fastq(), &impl_);
    return *this;
}

string BamRecord::MovieName(void) const
{ return ReadGroup().MovieName(); }

int32_t BamRecord::NumPasses(void) const
{
    const Tag& numPasses = impl_.TagValue(internal::tagName_numPasses);
    return numPasses.ToInt32();
}

BamRecord& BamRecord::NumPasses(const int32_t numPasses)
{
    internal::CreateOrEdit(internal::tagName_numPasses, numPasses, &impl_);
    return *this;
}

std::vector<float> BamRecord::Pkmean(Orientation orientation) const
{
    return FetchPhotons(internal::tagName_pkmean, orientation);
}

BamRecord& BamRecord::Pkmean(const std::vector<float>& photons)
{
    Pkmean(EncodePhotons(photons));
    return *this;
}

BamRecord& BamRecord::Pkmean(const std::vector<uint16_t>& encodedPhotons)
{
    internal::CreateOrEdit(internal::tagName_pkmean, encodedPhotons, &impl_);
    return *this;
}

std::vector<float> BamRecord::Pkmid(Orientation orientation) const
{
    return FetchPhotons(internal::tagName_pkmid, orientation);
}

BamRecord& BamRecord::Pkmid(const std::vector<float>& photons)
{
    Pkmid(EncodePhotons(photons));
    return *this;
}

BamRecord& BamRecord::Pkmid(const std::vector<uint16_t>& encodedPhotons)
{
    internal::CreateOrEdit(internal::tagName_pkmid, encodedPhotons, &impl_);
    return *this;
}

Frames BamRecord::PrePulseFrames(Orientation orientation) const
{
    return FetchFrames(internal::tagName_pre_pulse_frames, orientation);
}

BamRecord& BamRecord::PrePulseFrames(const Frames& frames,
                                     const FrameEncodingType encoding)
{
    if (encoding == FrameEncodingType::LOSSY)
        internal::CreateOrEdit(internal::tagName_pre_pulse_frames, frames.Encode(), &impl_);
    else
        internal::CreateOrEdit(internal::tagName_pre_pulse_frames, frames.Data(), &impl_);
    return *this;
}

std::string BamRecord::PulseCall(Orientation orientation) const
{
    return FetchBases(internal::tagName_pulse_call, orientation);
}

BamRecord& BamRecord::PulseCall(const std::string& tags)
{
    internal::CreateOrEdit(internal::tagName_pulse_call, tags, &impl_);
    return *this;
}

Frames BamRecord::PulseCallWidth(Orientation orientation) const
{
    return FetchFrames(internal::tagName_pulse_call_width, orientation);
}

BamRecord& BamRecord::PulseCallWidth(const Frames& frames,
                                     const FrameEncodingType encoding)
{
    if (encoding == FrameEncodingType::LOSSY)
        internal::CreateOrEdit(internal::tagName_pulse_call_width, frames.Encode(), &impl_);
    else
        internal::CreateOrEdit(internal::tagName_pulse_call_width, frames.Data(), &impl_);
    return *this;
}

Frames BamRecord::PulseWidth(Orientation orientation,
                             bool aligned,
                             bool exciseSoftClips) const
{
    return FetchFrames(internal::tagName_pulseWidth,
                       orientation,
                       aligned,
                       exciseSoftClips);
}

BamRecord& BamRecord::PulseWidth(const Frames& frames,
                                 const FrameEncodingType encoding)
{
    if (encoding == FrameEncodingType::LOSSY)
        internal::CreateOrEdit(internal::tagName_pulseWidth, frames.Encode(), &impl_);
    else
        internal::CreateOrEdit(internal::tagName_pulseWidth, frames.Data(), &impl_);
    return *this;
}

QualityValues BamRecord::Qualities(Orientation orientation,
                                   bool aligned,
                                   bool exciseSoftClips) const
{
    return FetchQualities("QUAL",
                          orientation,
                          aligned,
                          exciseSoftClips);
}

Position BamRecord::QueryEnd(void) const
{
    // try 'qe' tag
    const Tag& qe = impl_.TagValue(internal::tagName_queryEnd);
    if (!qe.IsNull())
        return qe.ToInt32();

    // tag missing, need to check movie name (fallback for non-PB BAMs, but ignore for CCS reads)
    RecordType type;
    try {
        type = Type();
    } catch (std::exception&) {
        return Position(0);
    }
    if (type == RecordType::CCS)
        throw std::runtime_error("no query end for CCS read type");

    // PacBio BAM, non-CCS
    try {
        return internal::QueryEndFromName(FullName());
    } catch (std::exception&) {
        // return fallback position
        return Position(0);
    }
}

BamRecord& BamRecord::QueryEnd(const Position pos)
{
   internal::CreateOrEdit(internal::tagName_queryEnd,
                          (int32_t)pos,
                          &impl_);
   UpdateName();
   return *this;
}

Position BamRecord::QueryStart(void) const
{
    // try 'qs' tag
    const Tag& qs = impl_.TagValue(internal::tagName_queryStart);
    if (!qs.IsNull())
        return qs.ToInt32();

    // tag missing, need to check movie name (fallback for non-PB BAMs, but ignore for CCS reads)
    RecordType type;
    try {
        type = Type();
    } catch (std::exception&) {
        return Position(0);
    }
    if (type == RecordType::CCS)
        throw std::runtime_error("no query start for CCS read type");

    // PacBio BAM, non-CCS
    try {
        return internal::QueryStartFromName(FullName());
    } catch (std::exception&) {
        // return fallback position
        return Position(0);
    }
}

BamRecord& BamRecord::QueryStart(const Position pos)
{
   internal::CreateOrEdit(internal::tagName_queryStart,
                          (int32_t)pos,
                          &impl_);
   UpdateName();
   return *this;
}


Accuracy BamRecord::ReadAccuracy(void) const
{
    const Tag& readAccuracy = impl_.TagValue(internal::tagName_readAccuracy);
    return Accuracy(readAccuracy.ToInt32());
}

BamRecord& BamRecord::ReadAccuracy(const Accuracy& accuracy)
{
    internal::CreateOrEdit(internal::tagName_readAccuracy,
                           static_cast<int32_t>(accuracy),
                           &impl_);
    return *this;
}

ReadGroupInfo BamRecord::ReadGroup(void) const
{ return header_.ReadGroup(ReadGroupId()); }

BamRecord& BamRecord::ReadGroup(const ReadGroupInfo& rg)
{
   internal::CreateOrEdit(internal::tagName_readGroup, rg.Id(), &impl_);
   UpdateName();
   return *this;
}

string BamRecord::ReadGroupId(void) const
{
    const Tag& rgTag = impl_.TagValue(internal::tagName_readGroup);
    if (rgTag.IsNull())
        return string();
    return rgTag.ToString();
}

BamRecord& BamRecord::ReadGroupId(const std::string& id)
{
   internal::CreateOrEdit(internal::tagName_readGroup,
                          id,
                          &impl_);
   UpdateName();
   return *this;
}

Position BamRecord::ReferenceEnd(void) const
{
    if (!impl_.IsMapped())
        return PacBio::BAM::UnmappedPosition;
    PBBAM_SHARED_PTR<bam1_t> htsData = internal::BamRecordMemory::GetRawData(impl_);
    if (!htsData)
        return PacBio::BAM::UnmappedPosition;
    return bam_endpos(htsData.get());
}

int32_t BamRecord::ReferenceId(void) const
{ return impl_.ReferenceId(); }

std::string BamRecord::ReferenceName(void) const
{
    if (IsMapped())
        return Header().SequenceName(ReferenceId());
    else
        throw std::runtime_error("unmapped record has no associated reference name");
}

Position BamRecord::ReferenceStart(void) const
{ return impl_.Position(); }

void BamRecord::ResetCachedPositions(void) const
{
    alignedEnd_   = PacBio::BAM::UnmappedPosition;
    alignedStart_ = PacBio::BAM::UnmappedPosition;
}

void BamRecord::ResetCachedPositions(void)
{
    alignedEnd_   = PacBio::BAM::UnmappedPosition;
    alignedStart_ = PacBio::BAM::UnmappedPosition;
}

VirtualRegionType BamRecord::ScrapType(void) const
{
    const Tag& scTag = impl_.TagValue(internal::tagName_scrap_type);
    return VirtualRegionTypeMap::ParseChar[scTag.ToUInt8()];
}

BamRecord& BamRecord::ScrapType(const VirtualRegionType type)
{
    internal::CreateOrEdit(internal::tagName_scrap_type,
                           static_cast<uint8_t>(type), &impl_);
    return *this;
}

BamRecord& BamRecord::ScrapType(const char type)
{
    internal::CreateOrEdit(internal::tagName_scrap_type, type, &impl_);
    return *this;
}

std::string BamRecord::Sequence(const Orientation orientation,
                                bool aligned,
                                bool exciseSoftClips) const
{
    return FetchBases("SEQ",
                      orientation,
                      aligned,
                      exciseSoftClips);
}

vector<float> BamRecord::SignalToNoise(void) const
{
    const Tag& snTag = impl_.TagValue(internal::tagName_snr);
    return snTag.ToFloatArray();
}

BamRecord& BamRecord::SignalToNoise(const vector<float>& snr)
{
    internal::CreateOrEdit(internal::tagName_snr, snr, &impl_);
    return *this;
}

QualityValues BamRecord::SubstitutionQV(Orientation orientation,
                                        bool aligned,
                                        bool exciseSoftClips) const
{
    return FetchQualities(internal::tagName_substitutionQV,
                          orientation,
                          aligned,
                          exciseSoftClips);
}

BamRecord& BamRecord::SubstitutionQV(const QualityValues& substitutionQVs)
{
    internal::CreateOrEdit(internal::tagName_substitutionQV, substitutionQVs.Fastq(), &impl_);
    return *this;
}



std::string BamRecord::SubstitutionTag(Orientation orientation,
                                        bool aligned,
                                        bool exciseSoftClips) const
{
    return FetchBases(internal::tagName_substitutionTag,
                      orientation,
                      aligned,
                      exciseSoftClips);
}

BamRecord& BamRecord::SubstitutionTag(const std::string& tags)
{
    internal::CreateOrEdit(internal::tagName_substitutionTag, tags, &impl_);
    return *this;
}

RecordType BamRecord::Type(void) const
{
    try {
        const string& typeName = ReadGroup().ReadType();
        return internal::NameToType(typeName);
    } catch (std::exception&) {

        // read group not found
        // peek at name to see if we're CCS
        if (FullName().find("ccs") != string::npos)
            return RecordType::CCS;

        // otherwise unknown
        else
            return RecordType::UNKNOWN;
    }
}

void BamRecord::UpdateName()
{
    std::string newName;
    newName.reserve(100);

    newName += MovieName();
    newName += "/";

    if (HasHoleNumber())
        newName += std::to_string(HoleNumber());
    else
        newName += "?";

    newName += "/";

    if (Type() == RecordType::CCS)
        newName += "ccs";
    else {
        if (HasQueryStart())
            newName += std::to_string(QueryStart());
        else
            newName += "?";

        newName += '_';

        if (HasQueryEnd())
            newName += std::to_string(QueryEnd());
        else
            newName += "?";
    }

    impl_.Name(newName);
}
