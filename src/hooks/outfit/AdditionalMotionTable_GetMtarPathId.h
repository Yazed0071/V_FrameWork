#pragma once

namespace outfit
{
    bool Install_OutfitMotionMtar_Hook();
    void Uninstall_OutfitMotionMtar_Hook();
    void RequestAdditionalMotionReresolve();
    void RevertAdditionalMotionSwaps();
    void NotePartsPipelineBusy(bool busy);
    void NoteLiveOutfitIdentity(unsigned char partsType, bool settled);
    void* RedirectMotionMtarForClip(void* mtar);
    bool  ConsumeAnimControlCensusRequest();
    void  NoteAnimControl(void* animControl);
}
