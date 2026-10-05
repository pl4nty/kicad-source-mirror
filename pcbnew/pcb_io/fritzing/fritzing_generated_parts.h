/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Footprint geometry follows Fritzing's own generators (PinHeader::makePcbSvg and
 * MysteryPart::makePcbDipSvg in https://github.com/fritzing/fritzing-app).
 */

#ifndef FRITZING_GENERATED_PARTS_H_
#define FRITZING_GENERATED_PARTS_H_

#include <string>

#include <wx/string.h>

#include "fritzing_model.h"


/**
 * Fritzing makes some parts on the fly from their module id (e.g.
 * "generic_female_pin_header_4_100mil" or "generic_ic_dip_8_300mil"), so they exist in
 * neither the sketch nor the parts library.  Recreate the definition and PCB image of the
 * through-hole pin header, SIP and DIP families.
 *
 * @return false if aModuleId is not a generated part this function knows.
 */
bool GenerateFritzingPart( const wxString& aModuleId, FRITZING::PART& aPart, std::string& aPcbSvg );

/**
 * Recreate a PCB image Fritzing generates from its file name, e.g. "pcb/dip_8_300mil_pcb.svg"
 * referenced by a stock DIP part.  Covers DIPs, SIPs, pin headers, mystery parts and screw
 * terminals.
 */
bool GenerateFritzingPcbSvg( const wxString& aImage, std::string& aSvg );

#endif // FRITZING_GENERATED_PARTS_H_
