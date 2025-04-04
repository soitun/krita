/*
 *  SPDX-FileCopyrightText: 2007 Cyrille Berger <cberger@cberger.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include "KoColorProofingConversionTransformation.h"

#include "KoColorSpace.h"


KoColorProofingConversionTransformation::KoColorProofingConversionTransformation(const KoColorSpace* srcCs,
                                                                 const KoColorSpace* dstCs,
                                                                 const KoColorSpace* proofingSpace,
                                                                 Intent renderingIntent,
                                                                 Intent proofingIntent,
                                                                 bool bcpFirstTransform,
                                                                 quint8 *gamutWarning,
                                                                 double adaptationState,
                                                                 ConversionFlags conversionFlags)
    : KoColorConversionTransformation(srcCs, dstCs, renderingIntent, conversionFlags)
{
    Q_ASSERT(proofingSpace);
    m_proofingIntent = proofingIntent;
    m_gamutWarning = gamutWarning;
    m_adaptationState = adaptationState;
    m_proofingSpace = proofingSpace;
    m_firstTransformBCP = bcpFirstTransform;
}

KoColorProofingConversionTransformation::~KoColorProofingConversionTransformation()
{
}

const KoColorSpace* KoColorProofingConversionTransformation::proofingSpace() const
{
    return m_proofingSpace;
}
