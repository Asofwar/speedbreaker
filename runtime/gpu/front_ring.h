// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The renderer's front images: which one a swap writes. Vulkan-free:
// tests/gpu_front_ring_test.cpp checks it alone; video/presenter.cpp's
// ClaimFrontImage picks with it (under the lock the presenter takes frames
// under), and gpu/renderer.cpp's PresentFrontBuffer writes the image picked.
//
// Up to v0.1.4 each size had one front image, written at every swap. The
// presenter takes a frame's image (RunFrame) and only queues its read of it
// after vkWaitForFences and vkAcquireNextImageKHR, which with a full FIFO
// queue wait until the next refresh. A swap in that window queued frame
// N+1's write into the image before the presenter's read: N+1 was shown in
// N's place, then again as itself, and N never. Neither the dropped nor the
// repeated count saw it (the presenter still took one frame per refresh),
// nor the frame-time graph. With the guest vblank locked to the display
// (video/vblank_lock.h) the swap comes about 1 ms after the refresh and the
// race was rarely lost; without the lock (VRR, 90/120/144 Hz, ProMotion)
// the phase drifts and it was lost again and again.
//
// Now each size has a ring of images, and a swap writes one the presenter
// doesn't hold: not the image of the frame it took last (it may not have
// queued its read yet, and it draws that frame again under the UI while the
// game presents nothing new), and not the one waiting to be taken, while
// another is free. Two are enough: the presenter holds at most one and at
// most one waits. When both are in use (the game made two frames within one
// of the presenter's: a hitch on the main thread) the waiting frame is
// taken back for the new one. It would have been replaced by it a moment
// later anyway, a frame dropped as before, and is counted and logged as
// one. A third image would only keep that frame takeable for the moment the
// swap takes to record, for another whole front image (33 MB at a 2160p
// internal resolution, out of a Steam Deck's 16 GB shared with the CPU).
#pragma once

namespace gpu
{
    // Front images a ring has (NFSMW_FRONT_IMAGES: 1 to kMaxFrontImages; 1
    // is v0.1.4's single image, whose lost races the presenter now logs).
    constexpr int kFrontImages = 2;
    constexpr int kMaxFrontImages = 3;

    struct FrontChoice
    {
        enum class Kind
        {
            Free,       // neither the presenter's nor waiting to be taken
            TakenBack,  // the waiting frame's: that frame is never shown (dropped)
            Presenter,  // the presenter's own (a ring of one): its read may not be queued yet
        };
        int index = 0;
        Kind kind = Kind::Free;
    };

    // The image of `images` (`count` of them, at least 1) the next swap
    // writes. `presenter`: the image of the frame the presenter took last
    // (null, or another ring's: none of these); `waiting`: the image of the
    // frame handed over and not yet taken (likewise). A free one first, then
    // the waiting frame's, and the presenter's only when there is no other.
    template <typename Handle>
    FrontChoice ChooseFrontImage(const Handle* images, int count, Handle presenter, Handle waiting)
    {
        for (int i = 0; i < count; i++)
            if (images[i] != presenter && images[i] != waiting)
                return { i, FrontChoice::Kind::Free };
        for (int i = 0; i < count; i++)
            if (images[i] != presenter)
                return { i, FrontChoice::Kind::TakenBack };
        return { 0, FrontChoice::Kind::Presenter };
    }
}
