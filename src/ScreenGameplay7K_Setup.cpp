#include "pch.h"

#include "GameGlobal.h"
#include "GameState.h"
#include "Logging.h"
#include "SongLoader.h"
#include "Screen.h"
#include "Audio.h"
#include "GameWindow.h"
#include "ImageList.h"

#include "SceneEnvironment.h"

#include "ScoreKeeper7K.h"
#include "TrackNote.h"
#include "ScreenGameplay7K.h"
#include "ScreenGameplay7K_Mechanics.h"

#include "AudioSourceOJM.h"
#include "BackgroundAnimation.h"
#include "Noteskin.h"
#include "Line.h"

#include "NoteTransformations.h"

CfgVar DisableBGA("DisableBGA");
CfgVar DisableKeysounds("DisableKeysounds");


ScreenGameplay7K::ScreenGameplay7K() : Screen("ScreenGameplay7K")
{
    SpeedMultiplier = 0;
    SongOldTime = -1;
    Music = nullptr;
    MechanicsSet = nullptr;
    GameTime = 0;
    Speed = 1;

    waveEffectEnabled = false;
    waveEffect = 0;
    WaitingTime = 1.5;

    stage_failed = false;

    NoFail = true;

    SelectedHiddenMode = HM_NONE; // No Hidden
    RealHiddenMode = HM_NONE;
    HideClampSum = 0;

    AudioStart = 0;

#if (defined WIN32) && (!defined NDEBUG)
    Auto = true;
#else
    Auto = false;
#endif

    perfect_auto = true;

    lifebar_type = LT_GROOVE;
    scoring_type = ST_IIDX;

    SpeedMultiplierUser = 4;
    SongFinished = false;

    CurrentVertical = 0;
    SongTime = 0;
    beatScrollEffect = 0;
    Random = 0;
    SongTimeReal = 0;

    AudioCompensation = (Configuration::GetConfigf("AudioCompensation") != 0);
    TimeCompensation = 0;

    InterpolateTime = (Configuration::GetConfigf("InterpolateTime") != 0);

    MissTime = 0;
    SuccessTime = 0;
    LoadedSong = nullptr;
    Active = false;
    Barline = nullptr;

    // Don't play unless everything goes right (later checks)
    DoPlay = false;
}

void ScreenGameplay7K::Cleanup()
{
    if (Music)
        Music->Stop();

    GameState::GetInstance().SetScorekeeper7K(nullptr);
    Noteskin::Cleanup();
}

void ScreenGameplay7K::AssignMeasure(uint32_t Measure)
{
    double Beat = 0;

    if (Measure == -1) // Run from within.
        return;

    if (!Measure)
    {
        ForceActivation = true;
        return;
    }

    for (auto i = 0U; i < Measure; i++)
        Beat += CurrentDiff->Data->Measures[i].Length;

    Log::Logf("Warping to measure measure %d at beat %f.\n", Measure, Beat);

    double Time = TimeAtBeat(CurrentDiff->Timing, CurrentDiff->Offset, Beat)
        + StopTimeAtBeat(CurrentDiff->Data->Stops, Beat);

    // Disable all notes before the current measure.
    for (auto k = 0U; k < CurrentDiff->Channels; k++)
    {
        for (auto m = NotesByChannel[k].begin(); m != NotesByChannel[k].end(); )
        {
            if (m->GetStartTime() < Time)
            {
                m = NotesByChannel[k].erase(m);
                if (m == NotesByChannel[k].end()) break;
                else continue;
            }
            ++m;
        }
    }

    // Remove non-played objects
    while (BGMEvents.size() && BGMEvents.front() <= Time)
    {
        BGMEvents.pop();
    }

    SongTime = SongTimeReal = Time;

    if (Music)
    {
        Log::Printf("Setting stream to time %f.\n", Time);
        SongOldTime = -1;
        Music->SeekTime(Time);
    }

    Active = true;
}

void ScreenGameplay7K::Init(std::shared_ptr<VSRG::Song> S, int DifficultyIndex, const GameParameters &Param)
{
    MySong = S;
    CurrentDiff = S->Difficulties[DifficultyIndex];

    Upscroll = Param.Upscroll;
    StartMeasure = Param.StartMeasure;
    waveEffectEnabled = Param.Wave;
    SelectedHiddenMode = (EHiddenMode)(int)Clamp((int)Param.HiddenMode, (int)HM_NONE, (int)HM_FLASHLIGHT);
    Preloaded = Param.Preloaded;
    Auto = Param.Auto;
    Speed = Param.Rate;
    NoFail = Param.NoFail;
    Random = Param.Random;
	RequestedLifebar = (LifeType)(int)Clamp((int)Param.GaugeType, (int)LT_AUTO, (int)LT_O2JAM);
	RequestedSystem = (int)Clamp(Param.SystemType, (int)VSRG::TI_NONE, (int)VSRG::TI_RAINDROP);
    ForceActivation = false;

    if (Param.StartMeasure == -1 && Auto)
        StartMeasure = 0;

    ScoreKeeper = std::make_shared<ScoreKeeper7K>();
    GameState::GetInstance().SetScorekeeper7K(ScoreKeeper);
}

void ScreenGameplay7K::CalculateHiddenConstants()
{
    /*
        Given the top of the screen being 1, the bottom being -1
        calculate the range for which the current hidden mode is defined.
    */
    const float FlashlightRatio = 0.25;
    float Center;

    // Hidden calc
    if (SelectedHiddenMode)
    {
        float LimPos = -((JudgmentLinePos / ScreenHeight) * 2 - 1); // Frac. of screen
        float AdjustmentSize;

        if (Upscroll)
        {
            Center = -((((ScreenHeight - JudgmentLinePos) / 2 + JudgmentLinePos) / ScreenHeight) * 2 - 1);

            // AdjustmentSize = -( ((ScreenHeight - JudgmentLinePos) / 2 / ScreenHeight) - 1 ); // A quarter of the playing field.
            AdjustmentSize = 0.1f;

            if (SelectedHiddenMode == 2)
            {
                HideClampHigh = Center;
                HideClampLow = -1 + AdjustmentSize;
            }
            else if (SelectedHiddenMode == 1)
            {
                HideClampHigh = LimPos - AdjustmentSize;
                HideClampLow = Center;
            }

            // Invert Hidden Mode.
            if (SelectedHiddenMode == HM_SUDDEN) RealHiddenMode = HM_HIDDEN;
            else if (SelectedHiddenMode == HM_HIDDEN) RealHiddenMode = HM_SUDDEN;
            else RealHiddenMode = SelectedHiddenMode;
        }
        else
        {
            Center = -((JudgmentLinePos / 2 / ScreenHeight) * 2 - 1);

            AdjustmentSize = 0.1f;
            // AdjustmentSize = -( ((JudgmentLinePos) / 2 / ScreenHeight) - 1 ); // A quarter of the playing field.

            // Hidden/Sudden
            if (SelectedHiddenMode == 2)
            {
                HideClampHigh = 1 - AdjustmentSize;
                HideClampLow = Center;
            }
            else if (SelectedHiddenMode == 1)
            {
                HideClampHigh = Center;
                HideClampLow = LimPos + AdjustmentSize;
            }

            RealHiddenMode = SelectedHiddenMode;
        }

        if (SelectedHiddenMode == HM_FLASHLIGHT) // Flashlight
        {
            HideClampLow = Center - FlashlightRatio;
            HideClampHigh = Center + FlashlightRatio;
            HideClampSum = -Center;
            HideClampFactor = 1 / FlashlightRatio;
        }
        else // Hidden/Sudden
        {
            HideClampSum = -HideClampLow;
            HideClampFactor = 1 / (HideClampHigh + HideClampSum);
        }
    }
}

bool ScreenGameplay7K::LoadChartData()
{
    uint8_t index = 0;
    if (!Preloaded)
    {
        // The difficulty details are destroyed; which means we should load this from its original file.
        SongLoader Loader(GameState::GetInstance().GetSongDatabase());
        std::filesystem::path FN;

        Log::Printf("Loading Chart...");
        LoadedSong = Loader.LoadFromMeta(MySong.get(), CurrentDiff, FN, index);

        if (LoadedSong == nullptr)
        {
            Log::Printf("Failure to load chart. (Filename: %s)\n", Utility::ToU8(FN.wstring()).c_str());
            return false;
        }

        MySong = LoadedSong;

        /*
            At this point, MySong == LoadedSong, which means it's not a metadata-only Song* Instance.
            The old copy is preserved; but this new one (LoadedSong) will be removed by the end of ScreenGameplay7K.
        */
    }

    BGA = BackgroundAnimation::CreateBGAFromSong(index, *MySong, this);

    return true;
}

bool ScreenGameplay7K::LoadSongAudio()
{
    if (!Music)
    {
        Music = std::make_shared<AudioStream>();
        Music->SetPitch(Speed);
        if (std::filesystem::exists(MySong->SongFilename)
			&& Music->Open(MySong->SongDirectory / MySong->SongFilename))
        {
            Log::Printf("Stream for %s succesfully opened.\n", MySong->SongFilename.c_str());
        }
        else
        {
            if (!CurrentDiff->IsVirtual)
            {
                // Caveat: Try to autodetect an mp3/ogg file.
                auto SngDir = MySong->SongDirectory;
                
                // Open the first MP3 and OGG file in the directory
                for (auto i: std::filesystem::directory_iterator(SngDir))
                {
                    auto extension = i.path().extension();
                    if (extension == ".mp3" || extension == ".ogg")
                        if (Music->Open(i.path()))
                            return true;
                }
                
                // Quit; couldn't find audio for a chart that requires it.
                Music = nullptr;
                Log::Printf("Unable to load song (Path: %s)\n", MySong->SongFilename.c_str());
                return false; 
            }
        }
    }

    // Load samples.
    if (MySong->SongFilename.extension() == ".ojm")
    {
        Log::Printf("Loading OJM.\n");
        OJMAudio = std::make_shared<AudioSourceOJM>(this);
        OJMAudio->SetPitch(Speed);
        OJMAudio->Open(MySong->SongDirectory / MySong->SongFilename);

        for (int i = 1; i <= 2000; i++)
        {
            std::shared_ptr<SoundSample> Snd = OJMAudio->GetFromIndex(i);

            if (Snd != nullptr)
                Keysounds[i].push_back(Snd);
        }
    }
    else if (CurrentDiff->SoundList.size() || CurrentDiff->Data->TimingInfo->GetType() == VSRG::TI_BMS)
    {
        Log::Printf("Loading samples... ");

        if (CurrentDiff->Data->TimingInfo->GetType() == VSRG::TI_BMS)
        {
            auto dir = MySong->SongDirectory;
            bool isBMSON = static_cast<VSRG::BMSTimingInfo*>(CurrentDiff->Data->TimingInfo.get())->IsBMSON;
            if (isBMSON)
            {
                int wavs = 0;
                std::map<int, SoundSample> audio;
                auto &slicedata = CurrentDiff->Data->SliceData;
                // do bmson loading
                for (auto wav : slicedata.Slices)
                {
                    for (auto sounds : wav.second)
                    {
                        CheckInterruption();
                        // load basic sound
                        if (!audio[sounds.first].IsValid())
                        {
                            auto path = (dir / slicedata.AudioFiles[sounds.first]);

                            audio[sounds.first].SetPitch(Speed);

                            if (!audio[sounds.first].Open(path))
                                throw std::runtime_error(Utility::Format("Unable to load %s.", slicedata.AudioFiles[sounds.first]).c_str());
                            Log::Printf("BMSON: Load sound %s\n", Utility::ToU8(path.wstring()).c_str());
                        }

                        audio[sounds.first].Slice(sounds.second.Start, sounds.second.End);
                        Keysounds[wav.first].push_back(audio[sounds.first].CopySlice());
                        wavs++;
                    }

                    Keysounds[wav.first].shrink_to_fit();
                }

                Log::Printf("BMSON: Generated %d sound objects.\n", wavs);
            }
        }

        for (auto i = CurrentDiff->SoundList.begin(); i != CurrentDiff->SoundList.end(); ++i)
        {
            auto ks = std::make_shared<SoundSample>();

            ks->SetPitch(Speed);
#ifdef WIN32
			std::filesystem::path rfd = i->second;
            std::filesystem::path afd = MySong->SongDirectory / rfd;
            ks->Open(afd);
#else
            ks->Open((MySong->SongDirectory.string() + "/" + i->second).c_str());
#endif
            Keysounds[i->first].push_back(ks);
            CheckInterruption();
        }
    }

    return true;
}

bool ScreenGameplay7K::ProcessSong()
{
    TimeCompensation = 0;

    double DesiredDefaultSpeed = Configuration::GetSkinConfigf("DefaultSpeedUnits");

    ESpeedType Type = (ESpeedType)(int)Configuration::GetSkinConfigf("DefaultSpeedKind");
    double SpeedConstant = 0; // Unless set, assume we're using speed changes

    int ApplyDriftVirtual = Configuration::GetConfigf("UseAudioCompensationKeysounds");
    int ApplyDriftDecoder = Configuration::GetConfigf("UseAudioCompensationNonKeysounded");

    if (AudioCompensation &&  // Apply drift is enabled and:
        ((ApplyDriftVirtual && CurrentDiff->IsVirtual) ||  // We want to apply it to a keysounded file and it's virtual
        (ApplyDriftDecoder && !CurrentDiff->IsVirtual))) // or we want to apply it to a non-keysounded file and it's not virtual
        TimeCompensation += MixerGetLatency();

    TimeCompensation += Configuration::GetConfigf("Offset7K");

    if (CurrentDiff->IsVirtual)
        TimeCompensation += Configuration::GetConfigf("OffsetKeysounded");
    else
        TimeCompensation += Configuration::GetConfigf("OffsetNonKeysounded");

    JudgeOffset = Configuration::GetConfigf("JudgeOffsetMS") / 1000;

    double Drift = TimeCompensation;

    Log::Logf("TimeCompensation: %f (Latency: %f / Offset: %f)\n", TimeCompensation, MixerGetLatency(), CurrentDiff->Offset);

    /*
 * 		There are three kinds of speed modifiers:
 * 			-CMod (Keep speed the same through the song, equal to a constant)
 * 			-MMod (Find highest speed and set multiplier to such that the highest speed is equal to a constant)
 *			-First (Find the first speed in the chart, and set multiplier to such that the first speed is equal to a constant)
 *
 *			The calculations are done ahead, and while SpeedConstant = 0 either MMod or first are assumed
 *			but only if there's a constant specified by the user.
 * */

 // What, you mean we don't have timing data at all?
    if (CurrentDiff->Timing.size() == 0)
    {
        Log::Printf("Error loading chart: No timing data.\n");
        return false;
    }

    Log::Printf("Processing song... ");

    if (DesiredDefaultSpeed)
    {
        if (Type == SPEEDTYPE_CMOD) // cmod
        {
            SpeedMultiplierUser = 1;
            SpeedConstant = DesiredDefaultSpeed;
        }

        CurrentDiff->GetPlayableData(NotesByChannel, BPS, VSpeeds, Warps, Drift, SpeedConstant);

        if (Type == SPEEDTYPE_MMOD) // mmod
        {
            double speed_max = 0; // Find the highest speed
            for (auto i = VSpeeds.begin();
            i != VSpeeds.end();
                ++i)
            {
                speed_max = std::max(speed_max, abs(i->Value));
            }

            double Ratio = DesiredDefaultSpeed / speed_max; // How much above or below are we from the maximum speed?
            SpeedMultiplierUser = Ratio;
        }
        else if (Type == SPEEDTYPE_FIRST) // We use this case as default. The logic is "Not a CMod, Not a MMod, then use first, the default.
        {
            double DesiredMultiplier = DesiredDefaultSpeed / VSpeeds[0].Value;

            SpeedMultiplierUser = DesiredMultiplier;
        } else if (Type == SPEEDTYPE_MODE)
        {
			std::map <double, double> freq;
			for (auto i = VSpeeds.begin(); i != VSpeeds.end(); i++)
			{
				if (i + 1 != VSpeeds.end())
				{
					freq[i->Value] += (i + 1)->Time - i->Time;
				}
				else freq[i->Value] += abs(CurrentDiff->Duration - i->Time);
			}
			auto max = -std::numeric_limits<float>::infinity();
			auto val = 1000.f;
			for (auto i: freq)
			{
				if (i.second > max)
				{
					max = i.second;
					val = i.first;
				}
			}

			SpeedMultiplierUser = DesiredDefaultSpeed / val;
        }
        else if (Type != SPEEDTYPE_CMOD) // other cases
        {
            double bpsd = 4.0 / (BPS[0].Value);
            double Speed = (MeasureBaseSpacing / bpsd);
            double DesiredMultiplier = DesiredDefaultSpeed / Speed;

            SpeedMultiplierUser = DesiredMultiplier;
        }
    }
    else
        CurrentDiff->GetPlayableData(NotesByChannel, BPS, VSpeeds, Warps, Drift); // Regular processing

    if (Type != SPEEDTYPE_CMOD)
        Speeds = CurrentDiff->Data->Speeds;

    for (auto&& w : Warps)
        w.Time += Drift;
    for (auto&& s : Speeds)
        s.Time += Drift;

    // Toggle whether we can use our guarantees for optimizations or not at rendering time.
    HasNegativeScroll = false;

    for (auto S : Speeds) if (S.Value < 0) HasNegativeScroll = true;
    for (auto S : VSpeeds) if (S.Value < 0) HasNegativeScroll = true;

    if (Random) NoteTransform::Randomize(NotesByChannel, CurrentDiff->Channels, CurrentDiff->Data->Turntable);

    // Load up BGM events
    auto BGMs = CurrentDiff->Data->BGMEvents;
    if (DisableKeysounds)
        NoteTransform::MoveKeysoundsToBGM(CurrentDiff->Channels, NotesByChannel, BGMs, Drift);

    sort(BGMs.begin(), BGMs.end());
    for (auto &s : BGMs)
        BGMEvents.push(s);

    return true;
}

bool ScreenGameplay7K::LoadBGA() const
{
	if (!DisableBGA)
	{
		try {
			BGA->Load();

			if (CurrentDiff->Data->osbSprites)
				CurrentDiff->Data->osbSprites = nullptr;
		} catch(std::exception &e)
		{
			Log::LogPrintf("Failure to load BGA: %s.\n", e.what());
		}
	}

    return true;
}

void ScreenGameplay7K::SetupAfterLoadingVariables()
{
    auto GearHeightFinal = Noteskin::GetJudgmentY();

    /* Initial object distance */
    if (!Upscroll)
        JudgmentLinePos = float(ScreenHeight) - GearHeightFinal;
    else
        JudgmentLinePos = GearHeightFinal;

    CurrentVertical = IntegrateToTime(VSpeeds, -WaitingTime);
    CurrentBeat = IntegrateToTime(BPS, 0);

    RecalculateMatrix();
    MultiplierChanged = true;

    CurrentDiff->GetMeasureLines(MeasureBarlines, VSpeeds, WaitingTime, TimeCompensation);

    ErrorTolerance = CfgVar("ErrorTolerance");

    if (ErrorTolerance <= 0)
        ErrorTolerance = 5; // ms
}

void ScreenGameplay7K::SetupMechanics()
{
    bool bmsOrStepmania = false;

    // This must be done before setLifeTotal in order for it to work.
    ScoreKeeper->setMaxNotes(CurrentDiff->TotalScoringObjects);

	// JudgeScale, Stepmania and OD can't be run together - only one can be set.
	auto TimingInfo = CurrentDiff->Data->TimingInfo.get();
	
	// Pick a timing system
	if (RequestedSystem == VSRG::TI_NONE) {
		if (TimingInfo) {
			// Automatic setup
			RequestedSystem = TimingInfo->GetType();
		}
		else {
			Log::Printf("Null timing info - assigning raindrop defaults.\n");
			RequestedSystem = VSRG::TI_RAINDROP;
			// pick raindrop system for null Timing Info
		}
	}

	// If we got just assigned one or was already requested
	// unlikely: timing info type is none? what
	retry:
	if (RequestedSystem != VSRG::TI_NONE) {
		// Player requested a specific subsystem
		if (RequestedSystem == VSRG::TI_BMS) {
			bmsOrStepmania = true;
			scoring_type = ST_EX;
			UsedTimingType = TT_TIME;
			if (TimingInfo->GetType() == VSRG::TI_BMS) {
				auto Info = static_cast<VSRG::BMSTimingInfo*> (TimingInfo);
				ScoreKeeper->setLifeTotal(Info->GaugeTotal);
			}
			else {
				ScoreKeeper->setJudgeRank(3);
			}
		}
		else if (RequestedSystem == VSRG::TI_O2JAM) {
			scoring_type = ST_O2JAM;
			UsedTimingType = TT_BEATS;
			ScoreKeeper->setJudgeRank(-100);
		}
		else if (RequestedSystem == VSRG::TI_OSUMANIA) {
			scoring_type = ST_OSUMANIA;
			UsedTimingType = TT_TIME;
			if (TimingInfo->GetType() == VSRG::TI_OSUMANIA) {
				auto InfoOM = static_cast<VSRG::OsuManiaTimingInfo*> (TimingInfo);
				ScoreKeeper->setODWindows(InfoOM->OD);
			}
			else ScoreKeeper->setODWindows(7);
		}
		else if (RequestedSystem == VSRG::TI_STEPMANIA) {
			bmsOrStepmania = true;
			UsedTimingType = TT_TIME;
			scoring_type = ST_DP;
			ScoreKeeper->setSMJ4Windows();
		}
		else if (RequestedSystem == VSRG::TI_RAINDROP) {
			scoring_type = ST_EXP3;
			lifebar_type = LT_STEPMANIA;
			bmsOrStepmania = true;
		}
	}
	else
	{
		Log::Printf("System picked was none - on purpose. Defaulting to raindrop.\n");
		RequestedSystem = VSRG::TI_RAINDROP;
		goto retry;
	}

	GameState::GetInstance().SetCurrentSystemType(RequestedSystem);

	// Timing System is set up. Set up life bar
	if (RequestedLifebar == LT_AUTO) {
		using namespace VSRG;
		switch (RequestedSystem) {
		case TI_BMS:
		case TI_RAINDROP:
			RequestedLifebar = LT_GROOVE;
			break;
		case TI_O2JAM:
			RequestedLifebar = LT_O2JAM;
			break;
		case TI_OSUMANIA:
		case TI_STEPMANIA:
			RequestedLifebar = LT_STEPMANIA;
			break;
		default:
			throw std::runtime_error("Invalid requested system.");
		}
	}

	switch (RequestedLifebar) {
	case LT_STEPMANIA:
		lifebar_type = LT_STEPMANIA; // Needs no setup.
		break;

	case LT_O2JAM:
		if (TimingInfo->GetType() == VSRG::TI_O2JAM) {
			auto InfoO2 = static_cast<VSRG::O2JamTimingInfo*> (TimingInfo);
			ScoreKeeper->setO2LifebarRating(InfoO2->Difficulty);
		} // else by default
		lifebar_type = LT_O2JAM; // By default, HX
		break;

	case LT_GROOVE:
	case LT_DEATH:
	case LT_EASY:
	case LT_EXHARD:
	case LT_SURVIVAL:
		if (TimingInfo->GetType() == VSRG::TI_BMS) { // Only needs setup if it's a BMS file
			auto Info = static_cast<VSRG::BMSTimingInfo*> (TimingInfo);
			ScoreKeeper->setLifeTotal(Info->GaugeTotal);
		}
		else // by raindrop defaults
			ScoreKeeper->setLifeTotal(-1);
		lifebar_type = (LifeType)RequestedLifebar;
		break;
	case LT_NORECOV:
		lifebar_type = LT_NORECOV;
		break;
	default:
		throw std::runtime_error("Invalid gauge type recieved");
	}

	/*
		If we're on TT_BEATS we've got to recalculate all note positions to beats,
		and use mechanics that use TT_BEATS as its timing type.
	*/

	GameState::GetInstance().SetCurrentGaugeType((int)lifebar_type);
	GameState::GetInstance().SetCurrentScoreType(scoring_type);

	if (UsedTimingType == TT_TIME)
	{
		Log::Printf("Using raindrop mechanics set!\n");
		// Only forced release if not a bms or a stepmania chart.
		MechanicsSet = std::make_shared<RaindropMechanics>(!bmsOrStepmania);
	}
	else if (UsedTimingType == TT_BEATS)
	{
		Log::Printf("Using o2jam mechanics set!\n");
		MechanicsSet = std::make_shared<O2JamMechanics>();
		NoteTransform::TransformToBeats(CurrentDiff->Channels, NotesByChannel, BPS);
	}

	MechanicsSet->Setup(MySong.get(), CurrentDiff.get(), ScoreKeeper);
	MechanicsSet->HitNotify = std::bind(&ScreenGameplay7K::HitNote, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);
	MechanicsSet->MissNotify = std::bind(&ScreenGameplay7K::MissNote, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5);
	MechanicsSet->IsLaneKeyDown = std::bind(&ScreenGameplay7K::GetGearLaneState, this, std::placeholders::_1);
	MechanicsSet->SetLaneHoldingState = std::bind(&ScreenGameplay7K::SetLaneHoldState, this, std::placeholders::_1, std::placeholders::_2);
	MechanicsSet->PlayLaneSoundEvent = std::bind(&ScreenGameplay7K::PlayLaneKeysound, this, std::placeholders::_1);
	MechanicsSet->PlayNoteSoundEvent = std::bind(&ScreenGameplay7K::PlayKeysound, this, std::placeholders::_1);

	// We're set - setup all of the variables that depend on mechanics, scoring etc.. to their initial values.
    UpdateScriptScoreVariables();
}

void ScreenGameplay7K::LoadResources()
{
	auto MissSndFile = Configuration::GetSkinSound("Miss");
	auto FailSndFile = Configuration::GetSkinSound("Fail");

	MissSnd.Open(MissSndFile);
	FailSnd.Open(FailSndFile);

	if (!LoadChartData() || !LoadSongAudio() || !ProcessSong() || !LoadBGA())
	{
		DoPlay = false;
		return;
	}

	SetupMechanics();
	TurntableEnabled = CurrentDiff->Data->Turntable;
	Noteskin::SetupNoteskin(CurrentDiff->Data->Turntable, CurrentDiff->Channels, this);

	SetupLua(Animations->GetEnv());
	SetupAfterLoadingVariables();
	SetupScriptConstants();
	UpdateScriptVariables();

	Animations->Preload(GameState::GetInstance().GetSkinFile("screengameplay7k.lua"), "Preload");
	Log::Printf("Done.\n");

	AssignMeasure(StartMeasure);

	ForceActivation = ForceActivation || (Configuration::GetSkinConfigf("InmediateActivation") == 1);

	// We're done with the data stored in the difficulties that aren't the one we're using. Clear it up.
	for (auto i = MySong->Difficulties.begin(); i != MySong->Difficulties.end(); ++i)
		(*i)->Destroy();

	DoPlay = true;
}

bool ScreenGameplay7K::BindKeysToLanes(bool UseTurntable)
{
	std::string KeyProfile;
	std::string value;
	std::vector<std::string> res;

	if (UseTurntable)
		KeyProfile = (std::string)CfgVar("KeyProfileSpecial" + Utility::IntToStr(CurrentDiff->Channels));
	else
		KeyProfile = (std::string)CfgVar("KeyProfile" + Utility::IntToStr(CurrentDiff->Channels));

	value = (std::string)CfgVar("Keys", KeyProfile);
	res = Utility::TokenSplit(value);

	for (unsigned i = 0; i < CurrentDiff->Channels; i++)
	{
		lastClosest[i] = 0;

		if (i < res.size())
			GearBindings[static_cast<int>(latof(res[i]))] = i;
		else
		{
			if (!Auto)
			{
				Log::Printf("Mising bindings starting from lane " + Utility::IntToStr(i) + " using profile " + KeyProfile);
				return false;
			}
		}

		HeldKey[i] = false;
		GearIsPressed[i] = 0;
	}

	return true;
}

void ScreenGameplay7K::InitializeResources()
{
	if (!BindKeysToLanes(TurntableEnabled))
		if (!BindKeysToLanes(!TurntableEnabled))
			DoPlay = false;

	if (!DoPlay) // Failure to load something important?
	{
		Running = false;
		return;
	}

	Noteskin::Validate();

	PlayReactiveSounds = (CurrentDiff->IsVirtual || !(Configuration::GetConfigf("DisableHitsounds")));
	MsDisplayMargin = (Configuration::GetSkinConfigf("HitErrorDisplayLimiter"));

	WindowFrame.SetLightMultiplier(0.75f);

	memset(CurrentKeysounds, 0, sizeof(CurrentKeysounds));

	CalculateHiddenConstants();

	if (!StartMeasure || StartMeasure == -1)
		WaitingTime = abs(std::min(-WaitingTime, CurrentDiff->Offset - 1.5));
	else
		WaitingTime = 0;

	if (Noteskin::IsBarlineEnabled())
		Barline = std::make_shared<Line>();

	CurrentBeat = IntegrateToTime(BPS, -WaitingTime);
	Animations->GetImageList()->ForceFetch();
	BGA->Validate();

	Animations->Initialize("", false);
	Running = true;
}
