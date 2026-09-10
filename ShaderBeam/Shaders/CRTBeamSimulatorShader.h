/*
ShaderBeam: shader effect overlay
Copyright (C) 2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderBeam
MIT License
*/

// From Shadertoy https://www.shadertoy.com/view/XfKfWd
// - Improved version coming January 2025
// - See accompanying article https://blurbusters.com/crt
// - To study more about display science & physics, see Research Portal https://blurbusters.com/area51
// adapted for ShaderBeam by mausimus

#pragma once

#include "SinglePassShaderProfile.h"

namespace ShaderBeam
{

class CRTBeamSimulatorShader : public SinglePassShaderProfile
{
public:
    struct
    {
        float gamma { 2.2f };
        float gainVsBlur { 0.5f };
        int   scanDirection { 1 };

        // CPU-calculated
        float effectiveFramesPerHz { 4.001f };
        float crtRasterPos { 0.0f };
        float crtHzCounter { 0.0f };
    } m_params;

    int   m_fpsDivisor { 1 };
    int   m_lcdAntiRetention { 1 };
    float m_lcdInversionCompensationSlew { 0.001f };
    int   m_beamSynchronousFrameUpdates { 1 };

    // derived
    float m_framesPerHz { 4.0f };

    const std::map<int, std::string> m_scanDirections = {
        { 0, "None (Global Refresh)" }, { 1, "Top to Bottom" }, { 2, "Bottom to Top" }, { 3, "Left to Right" }, { 4, "Right to Left" }
    };

    const std::map<int, std::string> m_lcdAntiRetentions = { { 0, "Force Off" }, { 1, "Auto" } };
    const std::map<int, std::string> m_frameUpdateModes = { { 0, "Frame Ahead (Legacy)" }, { 1, "Beam Synchronous" } };

    CRTBeamSimulatorShader()
    {
        m_numInputs = 3;
        m_name      = "Blur Busters CRT Beam Simulator";
        AddParameter("Gamma", "Your display's gamma value. Necessary to prevent horizontal-bands artifacts.", &m_params.gamma, 0.5f, 5.0f);
        AddParameter("Gain vs Blur",
                     "Brightness-vs-motionblur tradeoff for bright pixel.\n"
                     "- Defacto simulates fast/slow phosphor.\n"
                     "- 1.0 is unchanged brightness (same as non-CRT, but no blur reduction for brightest pixels, only for dimmer piels).\n"
                     "- 0.5 is half brightness spread over fewer frames (creates lower MPRT persistence for darker pixels).",
                     &m_params.gainVsBlur,
                     0.0f,
                     1.0f);
        AddParameter("Scan Direction",
                     "CRT SCAN DIRECTION. Can be useful to counteract an OS rotation of your display\n"
                     "'None' helps remove banding, but may reduce visual quality especially on OLEDs.",
                     &m_params.scanDirection,
                     0,
                     4,
                     m_scanDirections);
        AddParameter("Frame Update Mode",
                     "Controls how captured content advances while the simulated CRT raster is scanning.\n"
                     "- Beam Synchronous: the newest captured image is painted progressively by the beam while the previous image remains in the decaying phosphor region.\n"
                     "- Frame Ahead (Legacy): duplicates the newest capture across history for lower latency, so content can change globally.",
                     &m_beamSynchronousFrameUpdates,
                     0,
                     1,
                     m_frameUpdateModes);
        // NB: this is inverted compared to original shader to be more user-friendly
        AddParameter("Slow Motion Mode",
                     "Reduced frame rate mode\n"
                     "- This can be helpful to see individual CRT-simulated frames better (educational!)\n"
                     "- 1.0 is framerate=Hz, 2.0 is framerate being half of Hz, 10 is framerate being 10% of real Hz.\n",
                     &m_fpsDivisor,
                     1,
                     100);
        AddParameter("LCD Anti-retention",
                     "Prevents image retention from BFI interfering with LCD voltage polarity inversion algorithm\n"
                     "- It will cause occasional stutter as it desyncs CRT refresh rate from content refresh rate.\n"
                     "- Auto-disabled on OLEDs and LCDs with odd subframe count.\n"
                     "- Adds one input frame of latency (!)",
                     &m_lcdAntiRetention,
                     0,
                     1,
                     m_lcdAntiRetentions);
        AddParameter("LCD Inversion Compensation",
                     "Strength of LCD Anti-retention\n"
                     "- 0.001 - Normal\n"
                     "- 0.01 - Enhanced",
                     &m_lcdInversionCompensationSlew,
                     0.0f,
                     0.02f);
    }

    void Create(const RenderContext& renderContext)
    {
        m_framesPerHz = (float)renderContext.options.subFrames;

        // macros injected into the shader before compilation with non-adjustable constants (like resolution)
        D3D_SHADER_MACRO macros[2] = {
            { "HARDWARE_SRGB", renderContext.options.hardwareSrgb ? "1" : "0" },
            { NULL, NULL },
        };

        SetShader(L"Shaders\\CRTBeamSimulator.hlsl", macros, renderContext);
        SetParameterBuffer(&m_params, sizeof(m_params), renderContext);
        CreatePipeline(renderContext);
    }

    bool NewInputRequired(const RenderContext& renderContext) const
    {
        unsigned frameNo        = (renderContext.frameNo * renderContext.options.subFrames) + renderContext.subFrameNo;
        double   effectiveFrame = frameNo / (double)m_fpsDivisor;

        if(m_beamSynchronousFrameUpdates)
        {
            // The shader integrates every presented output over a one-frame-wide
            // interval [fStart, fStart + 1]. That means the visible leading edge
            // of the beam enters the NEXT CRT cycle one simulated native frame
            // before crtHzCounter itself rolls over. Prefetch/roll the capture at
            // that crossing so the new image begins at the beam's leading edge,
            // rather than appearing at its trailing edge one beam-width later.
            if(frameNo == 0)
                return true;

            double previousEffectiveFrame = (frameNo - 1) / (double)m_fpsDivisor;
            float  nextCycle               = (float)floor((effectiveFrame + 1.0) / m_params.effectiveFramesPerHz);
            float  previousNextCycle       = (float)floor((previousEffectiveFrame + 1.0) / m_params.effectiveFramesPerHz);
            return nextCycle != previousNextCycle;
        }

        // Preserve ShaderBeam's original capture timing for legacy mode.
        if(renderContext.frameNo == 0)
            return renderContext.subFrameNo == 0;

        float crtHzCounter = (float)floor(effectiveFrame / m_params.effectiveFramesPerHz);
        return crtHzCounter != m_params.crtHzCounter;
    }

    bool AntiRetentionRequired(const RenderContext& renderContext) const
    {
        return m_lcdAntiRetention && renderContext.options.monitorType == MONITOR_LCD && floorf(m_framesPerHz) == m_framesPerHz && (((int)m_framesPerHz) % 2) == 0;
    }

    bool SupportsResync(const RenderContext& renderContext) const
    {
        // Skipping raster subframes can move the beam without advancing the
        // preserved frame history, so disable automatic resync in authentic mode.
        return !m_beamSynchronousFrameUpdates && !AntiRetentionRequired(renderContext);
    }

    void OverrideInputs(const RenderContext& renderContext, const std::span<ID3D11ShaderResourceView*>& inputs)
    {
        if(m_beamSynchronousFrameUpdates)
        {
            // A CRT output sample covers a one-native-frame-wide interval. During
            // the final interval before a CRT-cycle rollover, the leading edge of
            // that interval has already wrapped to the top of the NEXT scan while
            // the trailing part of the OLD scan is still visible at the bottom.
            //
            // NewInputRequired() deliberately rolls the capture ring at the start
            // of this lookahead interval, so the raw ring is exactly what the
            // original Blur Busters temporal model needs here:
            //
            //   inputs[0] = next/new frame (pixelCurr; top seam / leading edge)
            //   inputs[1] = current/old frame (pixelPrev1; rest of current beam)
            //   inputs[2] = previous frame (pixelPrev2; older phosphor)
            //
            // Do NOT duplicate the newest frame during this interval or the old
            // beam/phosphor still visible at the bottom will change prematurely.
            float rasterFrame = m_params.crtRasterPos * m_params.effectiveFramesPerHz;
            bool  beamLookahead = (rasterFrame + 1.0f) >= m_params.effectiveFramesPerHz;

            if(!beamLookahead)
            {
                // Once the CRT counter has wrapped, the prefetched frame is now
                // the active beam frame. Until the next lookahead capture arrives,
                // duplicate it into pixelCurr and pixelPrev1 while keeping the old
                // active frame available to pixelPrev2 for phosphor decay.
                auto newest   = inputs[0];
                auto previous = inputs[1];
                inputs[0] = newest;
                inputs[1] = newest;
                inputs[2] = previous;
            }
        }
        else if(!AntiRetentionRequired(renderContext))
        {
            // Original ShaderBeam low-latency frame-ahead behavior.
            for(int slot = 1; slot < inputs.size(); slot++)
                inputs[slot] = inputs[slot - 1];
        }
    }

    void Render(const RenderContext& renderContext)
    {
        // linear frame number
        unsigned frameNo = (renderContext.frameNo * renderContext.options.subFrames) + renderContext.subFrameNo;

        //-------------------------------------------------------------------------------------------------
        // CRT beam calculations
        // Frame counter, which may be compensated by slo-mo modes (FPS_DIVISOR). Does not need to be integer divisible.
        double effectiveFrame = frameNo / (double)m_fpsDivisor;

        // LCD SAVER (prevent image retention)
        // Adds a slew to FRAMES_PER_HZ when ANTI_RETENTION is enabled and FRAMES_PER_HZ is an exact even integer.
        // We support non-integer FRAMES_PER_HZ, so this is a magically convenient solution
        m_params.effectiveFramesPerHz = (float)m_framesPerHz;
        if(AntiRetentionRequired(renderContext))
        {
            m_params.effectiveFramesPerHz += m_lcdInversionCompensationSlew;
        }

        // NB: have to use doubles here as fmodf and floorf could return inconsistent results on certain edge cases (frame 3910)

        // Normalized raster position [0..1] representing current position of simulated CRT electron beam
        m_params.crtRasterPos = (float)fmod(effectiveFrame, m_params.effectiveFramesPerHz) / m_params.effectiveFramesPerHz;

        // CRT refresh cycle counter
        m_params.crtHzCounter = (float)floor(effectiveFrame / m_params.effectiveFramesPerHz);

        UpdateParameters(renderContext);
        RenderPipeline(renderContext);
    }
};
} // namespace ShaderBeam