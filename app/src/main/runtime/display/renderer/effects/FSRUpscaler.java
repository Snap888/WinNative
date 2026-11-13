package com.winlator.cmod.runtime.display.renderer.effects;

/** AMD FidelityFX Super Resolution 1 spatial upscaler (EASU + RCAS). */
public final class FSRUpscaler extends Effect {
    private float renderScale = 0.67f;
    private float sharpness = 0.25f;

    public void setRenderScale(float value) {
        renderScale = Math.max(0.25f, Math.min(1.5f, value));
    }

    public void setSharpness(float value) {
        sharpness = Math.max(0.0f, Math.min(1.0f, value));
    }

    @Override
    public int getNativeType() {
        return TYPE_FSR;
    }

    @Override
    public float[] getParams() {
        return new float[]{0f, renderScale, sharpness, 0f};
    }

    @Override
    public void writeParams(float[] out, int offset) {
        out[offset] = 0f;
        out[offset + 1] = renderScale;
        out[offset + 2] = sharpness;
        out[offset + 3] = 0f;
    }
}