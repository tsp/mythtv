#include "DVDRingBuffer.h"
#include "DetectLetterbox.h"
#include "audiooutput.h"
#include "myth_imgconvert.h"
#include "mythdvdplayer.h"

#define LOC      QString("DVDPlayer: ")
#define LOC_WARN QString("DVDPlayer, Warning: ")
#define LOC_ERR  QString("DVDPlayer, Error: ")

MythDVDPlayer::MythDVDPlayer(bool muted)
  : MythPlayer(muted), m_buttonVersion(0),
    dvd_stillframe_showing(false), need_change_dvd_track(0),
    m_initial_title(-1), m_initial_audio_track(-1), m_initial_subtitle_track(-1),
    m_stillFrameLength(0)
{
}

void MythDVDPlayer::AutoDeint(VideoFrame *frame, bool allow_lock)
{
    MythPlayer::AutoDeint(frame, false);
}

void MythDVDPlayer::ReleaseNextVideoFrame(VideoFrame *buffer,
                                          int64_t timecode, bool wrap)
{
    MythPlayer::ReleaseNextVideoFrame(buffer, timecode,
                        !player_ctx->buffer->InDVDMenuOrStillFrame());
}

void MythDVDPlayer::DisableCaptions(uint mode, bool osd_msg)
{
    if ((kDisplayAVSubtitle & mode) && player_ctx->buffer->isDVD())
        player_ctx->buffer->DVD()->SetTrack(kTrackTypeSubtitle, -1);
    MythPlayer::DisableCaptions(mode, osd_msg);
}

void MythDVDPlayer::EnableCaptions(uint mode, bool osd_msg)
{
    if ((kDisplayAVSubtitle & mode) && player_ctx->buffer->isDVD())
        player_ctx->buffer->DVD()->SetTrack(kTrackTypeSubtitle,
                                            GetTrack(kTrackTypeSubtitle));
    MythPlayer::EnableCaptions(mode, osd_msg);
}

void MythDVDPlayer::DisplayPauseFrame(void)
{
    if (player_ctx->buffer->isDVD() &&
        player_ctx->buffer->DVD()->InStillFrame())
        SetScanType(kScan_Progressive);
    DisplayDVDButton();
    MythPlayer::DisplayPauseFrame();
}

void MythDVDPlayer::DecoderPauseCheck(void)
{
    StillFrameCheck();
    MythPlayer::DecoderPauseCheck();
}

bool MythDVDPlayer::PrebufferEnoughFrames(bool pause_audio, int  min_buffers)
{
    bool instill = player_ctx->buffer->InDVDMenuOrStillFrame();
    return MythPlayer::PrebufferEnoughFrames(!instill, 1);
}

bool MythDVDPlayer::DecoderGetFrameFFREW(void)
{
    if (decoder)
        decoder->UpdateDVDFramesPlayed();
    return MythPlayer::DecoderGetFrameFFREW();
}

bool MythDVDPlayer::DecoderGetFrameREW(void)
{
    MythPlayer::DecoderGetFrameREW();
    return (player_ctx->buffer->isDVD() &&
           (player_ctx->buffer->DVD()->GetCurrentTime() < 2));
}

void MythDVDPlayer::DisplayNormalFrame(bool allow_pause)
{
    bool allow = player_ctx->buffer->isDVD() &&
                (!player_ctx->buffer->InDVDMenuOrStillFrame() ||
                (player_ctx->buffer->DVD()->NumMenuButtons() > 0 &&
                 player_ctx->buffer->DVD()->GetChapterLength() > 3));
    MythPlayer::DisplayNormalFrame(allow);
}

void MythDVDPlayer::PreProcessNormalFrame(void)
{
    DisplayDVDButton();
}

bool MythDVDPlayer::VideoLoop(void)
{
    if (!player_ctx->buffer->isDVD())
    {
        SetErrored("RingBuffer is not a DVD.");
        return !IsErrored();
    }

    int nbframes = 0;
    if (videoOutput)
        nbframes = videoOutput->ValidVideoFrames();

    //VERBOSE(VB_PLAYBACK, LOC + QString("Validframes %1, FreeFrames %2, VideoPaused %3")
    //    .arg(nbframes).arg(videoOutput->FreeVideoFrames()).arg(videoPaused));

    // completely drain the video buffers for certain situations
    bool release_all = player_ctx->buffer->DVD()->DVDWaitingForPlayer() &&
                      (nbframes > 0);
    bool release_one = (nbframes > 1) && videoPaused &&
                       (!videoOutput->EnoughFreeFrames() ||
                        player_ctx->buffer->DVD()->IsWaiting() ||
                        player_ctx->buffer->DVD()->InStillFrame());
    if (release_all || release_one)
    {
        if (nbframes < 5 && videoOutput)
            videoOutput->UpdatePauseFrame();

        // if we go below the pre-buffering limit, the player will pause
        // so do this 'manually'
        DisplayNormalFrame(false);
        dvd_stillframe_showing = false;
        return !IsErrored();
    }

    // clear the mythtv imposed wait state
    if (player_ctx->buffer->DVD()->DVDWaitingForPlayer())
    {
        VERBOSE(VB_PLAYBACK, LOC + "Clearing Mythtv dvd wait state");
        player_ctx->buffer->DVD()->SkipDVDWaitingForPlayer();
        ClearAfterSeek(true);
        if (!player_ctx->buffer->DVD()->InStillFrame() && videoPaused)
            UnpauseVideo();
        return !IsErrored();
    }

    // wait for the video buffers to drain
    if (nbframes < 2)
    {
        // clear the DVD wait state
        if (player_ctx->buffer->DVD()->IsWaiting())
        {
            VERBOSE(VB_PLAYBACK, LOC + "Clearing DVD wait state");
            player_ctx->buffer->DVD()->WaitSkip();
            if (!player_ctx->buffer->DVD()->InStillFrame() && videoPaused)
                UnpauseVideo();
            return !IsErrored();
        }

        // we need a custom presentation method for still frame menus with audio
        if (player_ctx->buffer->DVD()->IsInMenu() &&
           !player_ctx->buffer->DVD()->InStillFrame())
        {
            DisplayLastFrame();
            return !IsErrored();
        }

        // the still frame is treated as a pause frame
        if (player_ctx->buffer->DVD()->InStillFrame())
        {
            // ensure we refresh the pause frame
            if (!dvd_stillframe_showing)
                needNewPauseFrame = true;

            // we are in a still frame so pause video output
            if (!videoPaused)
            {
                PauseVideo();
                return !IsErrored();
            }

            // see if the pause frame has timed out
            StillFrameCheck();

            // flag if we have no frame
            if (nbframes == 0)
            {
                VERBOSE(VB_PLAYBACK, LOC_WARN +
                        "In DVD Menu: No video frames in queue");
                usleep(10000);
                return !IsErrored();
            }

            dvd_stillframe_showing = true;
        }
        else
        {
            dvd_stillframe_showing = false;
        }
    }

    // unpause the still frame if more frames become available
    if (dvd_stillframe_showing && nbframes > 1)
    {
        UnpauseVideo();
        dvd_stillframe_showing = false;
        return !IsErrored();
    }

    if (videoPaused || isDummy)
    {
        usleep(frame_interval);
        DisplayPauseFrame();
    }
    else
        DisplayNormalFrame();

    if (using_null_videoout && decoder)
        decoder->UpdateFramesPlayed();
    else
        framesPlayed = videoOutput->GetFramesPlayed();
    return !IsErrored();
}

void MythDVDPlayer::DisplayLastFrame(void)
{
    videoOutput->StartDisplayingFrame();
    VideoFrame *frame = videoOutput->GetLastShownFrame();
    frame->timecode = audio.GetAudioTime();
    DisplayDVDButton();

    AutoDeint(frame);
    detect_letter_box->SwitchTo(frame);

    FrameScanType ps = m_scan;
    if (kScan_Detect == m_scan || kScan_Ignore == m_scan)
        ps = kScan_Progressive;

    videofiltersLock.lock();
    videoOutput->ProcessFrame(frame, osd, videoFilters, pip_players, ps);
    videofiltersLock.unlock();

    AVSync();
}

bool MythDVDPlayer::FastForward(float seconds)
{
    if (decoder)
        decoder->UpdateDVDFramesPlayed();
    return MythPlayer::FastForward(seconds);
}

bool MythDVDPlayer::Rewind(float seconds)
{
    if (decoder)
        decoder->UpdateDVDFramesPlayed();
    return MythPlayer::Rewind(seconds);
}

bool MythDVDPlayer::JumpToFrame(uint64_t frame)
{
    if (decoder)
        decoder->UpdateDVDFramesPlayed();
    return MythPlayer::JumpToFrame(frame);
}

void MythDVDPlayer::EventStart(void)
{
    if (player_ctx->buffer->DVD())
        player_ctx->buffer->DVD()->SetParent(this);
    MythPlayer::EventStart();
}

void MythDVDPlayer::EventLoop(void)
{
    MythPlayer::EventLoop();
    if (need_change_dvd_track)
    {
        DoChangeDVDTrack();
        need_change_dvd_track = 0;
    }
}

void MythDVDPlayer::InitialSeek(void)
{
    player_ctx->buffer->DVD()->IgnoreStillOrWait(true);
    if (m_initial_title > -1)
        player_ctx->buffer->DVD()->PlayTitleAndPart(m_initial_title, 1);

    if (m_initial_audio_track > -1)
        player_ctx->buffer->DVD()->SetTrack(kTrackTypeAudio,
                                            m_initial_audio_track);
    if (m_initial_subtitle_track > -1)
        player_ctx->buffer->DVD()->SetTrack(kTrackTypeSubtitle,
                                            m_initial_subtitle_track);

    if (bookmarkseek > 30)
    {

        // we need to trigger a dvd cell change to ensure the new title length
        // is set and the position map updated accordingly
        decodeOneFrame = true;
        int count = 0;
        while (count++ < 100 && decodeOneFrame)
            usleep(50000);
    }
    MythPlayer::InitialSeek();
    player_ctx->buffer->DVD()->IgnoreStillOrWait(false);
}

void MythDVDPlayer::ResetPlaying(bool resetframes)
{
    MythPlayer::ResetPlaying(false);
}

void MythDVDPlayer::EventEnd(void)
{
    if (player_ctx->buffer->DVD())
        player_ctx->buffer->DVD()->SetParent(NULL);
}

bool MythDVDPlayer::PrepareAudioSample(int64_t &timecode)
{
    if (!player_ctx->buffer->InDVDMenuOrStillFrame())
        WrapTimecode(timecode, TC_AUDIO);

    if (player_ctx->buffer->isDVD() &&
        player_ctx->buffer->DVD()->InStillFrame())
        return true;
    return false;
}

void MythDVDPlayer::SetBookmark(void)
{
    if (player_ctx->buffer->InDVDMenuOrStillFrame())
        SetDVDBookmark(0);
    else
    {
        SetDVDBookmark(framesPlayed);
        SetOSDStatus(QObject::tr("Position"), kOSDTimeout_Med);
        SetOSDMessage(QObject::tr("Bookmark Saved"), kOSDTimeout_Med);
    }
}

void MythDVDPlayer::ClearBookmark(bool message)
{
    SetDVDBookmark(0);
    if (message)
        SetOSDMessage(QObject::tr("Bookmark Cleared"), kOSDTimeout_Med);
}

uint64_t MythDVDPlayer::GetBookmark(void)
{
    if (gCoreContext->IsDatabaseIgnored() || !player_ctx->buffer->isDVD())
        return 0;

    QStringList dvdbookmark = QStringList();
    QString name;
    QString serialid;
    long long frames = 0;
    player_ctx->LockPlayingInfo(__FILE__, __LINE__);
    if (player_ctx->playingInfo)
    {
        if (!player_ctx->buffer->DVD()->GetNameAndSerialNum(name, serialid))
        {
            player_ctx->UnlockPlayingInfo(__FILE__, __LINE__);
            return 0;
        }
        dvdbookmark = player_ctx->playingInfo->QueryDVDBookmark(serialid,
                                                                false);
        if (!dvdbookmark.empty())
        {
            QStringList::Iterator it = dvdbookmark.begin();
            m_initial_title = (*it).toInt();
            frames = (long long)((*++it).toLongLong() & 0xffffffffLL);
            m_initial_audio_track    = (*++it).toInt();
            m_initial_subtitle_track = (*++it).toInt();
            VERBOSE(VB_PLAYBACK, LOC +
                QString("Get Bookmark: title %1 audiotrack %2 subtrack %3 frame %4")
                .arg(m_initial_title).arg(m_initial_audio_track)
                .arg(m_initial_subtitle_track).arg(frames));
        }
    }
    player_ctx->UnlockPlayingInfo(__FILE__, __LINE__);
    return frames;;
}

void MythDVDPlayer::ChangeSpeed(void)
{
    MythPlayer::ChangeSpeed();
    if (decoder)
        decoder->UpdateDVDFramesPlayed();
    if (play_speed != normal_speed && player_ctx->buffer->isDVD())
        player_ctx->buffer->DVD()->SetDVDSpeed(-1);
    else if (player_ctx->buffer->isDVD())
        player_ctx->buffer->DVD()->SetDVDSpeed();
}

void MythDVDPlayer::AVSync(bool limit_delay)
{
    MythPlayer::AVSync(true);
}

long long MythDVDPlayer::CalcMaxFFTime(long long ff, bool setjump) const
{
    if ((totalFrames > 0) && player_ctx->buffer->isDVD() &&
        player_ctx->buffer->DVD()->TitleTimeLeft() < 5)
        return 0;
    return MythPlayer::CalcMaxFFTime(ff, setjump);
}

void MythDVDPlayer::calcSliderPos(osdInfo &info, bool paddedFields)
{
    bool islive = false;
    info.text.insert("description", "");
    info.values.insert("position",   0);
    info.values.insert("progbefore", 0);
    info.values.insert("progafter",  0);
    if (!player_ctx->buffer->isDVD())
        return;

    int playbackLen = totalLength;
    float secsplayed = 0.0f;
#if !CONFIG_CYGWIN
    if (m_stillFrameLength > 0)
    {
        playbackLen = m_stillFrameLength;
        secsplayed  = m_stillFrameTimer.elapsed() / 1000;
    }
    else
    {
        secsplayed = player_ctx->buffer->DVD()->GetCurrentTime();
    }
#else
    // DVD playing non-functional under windows for now
    secsplayed = 0.0f;
#endif
    calcSliderPosPriv(info, paddedFields, playbackLen, secsplayed, islive);
}

void MythDVDPlayer::SeekForScreenGrab(uint64_t &number, uint64_t frameNum,
                                      bool absolute)
{
    if (!player_ctx->buffer->isDVD())
        return;
    if (GoToDVDMenu("menu"))
    {
        if (player_ctx->buffer->DVD()->IsInMenu() &&
           !player_ctx->buffer->DVD()->InStillFrame())
            GoToDVDProgram(1);
    }
    else if (player_ctx->buffer->DVD()->GetTotalTimeOfTitle() < 60)
    {
        GoToDVDProgram(1);
        number = frameNum;
        if (number >= totalFrames)
            number = totalFrames / 2;
    }
}

int MythDVDPlayer::SetTrack(uint type, int trackNo)
{
    if (kTrackTypeAudio == type)
        player_ctx->buffer->DVD()->SetTrack(type, trackNo);
    return MythPlayer::SetTrack(type, trackNo);
}

void MythDVDPlayer::ChangeDVDTrack(bool ffw)
{
    need_change_dvd_track = (ffw ? 1 : -1);
}

void MythDVDPlayer::DoChangeDVDTrack(void)
{
    SaveAudioTimecodeOffset(GetAudioTimecodeOffset());
    if (decoder)
        decoder->ChangeDVDTrack(need_change_dvd_track > 0);
    ClearAfterSeek(!player_ctx->buffer->InDVDMenuOrStillFrame());
}

void MythDVDPlayer::DisplayDVDButton(void)
{
    if (!osd || !player_ctx->buffer->isDVD())
        return;

    uint buttonversion = 0;
    AVSubtitle *dvdSubtitle = player_ctx->buffer->DVD()->GetMenuSubtitle(buttonversion);
    bool numbuttons    = player_ctx->buffer->DVD()->NumMenuButtons();

    // nothing to do
    if (buttonversion == m_buttonVersion)
    {
        player_ctx->buffer->DVD()->ReleaseMenuButton();
        return;
    }

    // clear any buttons
    if (!numbuttons || !dvdSubtitle || (buttonversion == 0))
    {
        SetCaptionsEnabled(false, false);
        if (osd)
            osd->ClearSubtitles();
        m_buttonVersion = 0;
        player_ctx->buffer->DVD()->ReleaseMenuButton();
        return;
    }

    m_buttonVersion = buttonversion;
    QRect buttonPos = player_ctx->buffer->DVD()->GetButtonCoords();
    osd->DisplayDVDButton(dvdSubtitle, buttonPos);
    textDisplayMode = kDisplayDVDButton;
    player_ctx->buffer->DVD()->ReleaseMenuButton();
}

bool MythDVDPlayer::GoToDVDMenu(QString str)
{
    if (!player_ctx->buffer->isDVD())
        return false;
    textDisplayMode = kDisplayNone;
    bool ret = player_ctx->buffer->DVD()->GoToMenu(str);

    if (!ret)
    {
        SetOSDMessage(QObject::tr("DVD Menu Not Available"), kOSDTimeout_Med);
        VERBOSE(VB_GENERAL, "No DVD Menu available.");
        return false;
    }

    return true;
}

void MythDVDPlayer::GoToDVDProgram(bool direction)
{
    if (!player_ctx->buffer->isDVD())
        return;
    if (direction == 0)
        player_ctx->buffer->DVD()->GoToPreviousProgram();
    else
        player_ctx->buffer->DVD()->GoToNextProgram();
}

void MythDVDPlayer::SetDVDBookmark(uint64_t frame)
{
    if (!player_ctx->buffer->isDVD())
        return;

    uint64_t framenum = frame;
    QStringList fields;
    QString name;
    QString serialid;
    int title = 0;
    int part;
    int audiotrack = -1;
    int subtitletrack = -1;
    if (!player_ctx->buffer->DVD()->GetNameAndSerialNum(name, serialid))
    {
        VERBOSE(VB_IMPORTANT, LOC +
                QString("DVD has no name and serial number. "
                        "Cannot set bookmark."));
        return;
    }

    if (!player_ctx->buffer->InDVDMenuOrStillFrame() &&
        player_ctx->buffer->DVD()->GetTotalTimeOfTitle() > 120 && frame > 0)
    {
        audiotrack = GetTrack(kTrackTypeAudio);
        if (GetCaptionMode() == kDisplayAVSubtitle)
        {
            subtitletrack = player_ctx->buffer->DVD()->GetTrack(
                kTrackTypeSubtitle);
        }
        player_ctx->buffer->DVD()->GetPartAndTitle(part, title);
    }
    else
        framenum = 0;

    player_ctx->LockPlayingInfo(__FILE__, __LINE__);
    if (player_ctx->playingInfo)
    {
        fields += serialid;
        fields += name;
        fields += QString("%1").arg(title);
        fields += QString("%1").arg(audiotrack);
        fields += QString("%1").arg(subtitletrack);
        fields += QString("%1").arg(framenum);
        player_ctx->playingInfo->SaveDVDBookmark(fields);
        VERBOSE(VB_PLAYBACK, LOC +
            QString("Set Bookmark: title %1 audiotrack %2 subtrack %3 frame %4")
            .arg(title).arg(audiotrack).arg(subtitletrack).arg(framenum));
    }
    player_ctx->UnlockPlayingInfo(__FILE__, __LINE__);
}

int MythDVDPlayer::GetNumAngles(void) const
{
    if (player_ctx->buffer->DVD() && player_ctx->buffer->DVD()->IsOpen())
        return player_ctx->buffer->DVD()->GetNumAngles();
    return 0;
}

int MythDVDPlayer::GetCurrentAngle(void) const
{
    if (player_ctx->buffer->DVD() && player_ctx->buffer->DVD()->IsOpen())
        return player_ctx->buffer->DVD()->GetCurrentAngle();
    return -1; 
}

QString MythDVDPlayer::GetAngleName(int angle) const
{
    if (angle >= 0 && angle < GetNumAngles())
    {
        QString name = QObject::tr("Angle %1").arg(angle+1);
        return name;
    }
    return QString();
}

bool MythDVDPlayer::SwitchAngle(int angle)
{
    uint total = GetNumAngles();
    if (!total || angle == GetCurrentAngle())
        return false;

    if (angle >= (int)total)
        angle = 0;

    return player_ctx->buffer->DVD()->SwitchAngle(angle);
}

void MythDVDPlayer::ResetStillFrameTimer(void)
{
    m_stillFrameTimerLock.lock();
    m_stillFrameTimer.restart();
    m_stillFrameTimerLock.unlock();
}

void MythDVDPlayer::SetStillFrameTimeout(int length)
{
    m_stillFrameLength = length;
}

void MythDVDPlayer::StillFrameCheck(void)
{
    if (player_ctx->buffer->isDVD() &&
        player_ctx->buffer->DVD()->InStillFrame() &&
       (m_stillFrameLength > 0) && (m_stillFrameLength < 0xff))
    {
        m_stillFrameTimerLock.lock();
        int elapsedTime = m_stillFrameTimer.elapsed() / 1000;
        m_stillFrameTimerLock.unlock();
        if (elapsedTime >= m_stillFrameLength)
        {
            VERBOSE(VB_PLAYBACK, LOC +
                QString("Stillframe timeout after %1 seconds")
                .arg(m_stillFrameLength));
            player_ctx->buffer->DVD()->SkipStillFrame();
            m_stillFrameLength = 0;
        }
    }
}