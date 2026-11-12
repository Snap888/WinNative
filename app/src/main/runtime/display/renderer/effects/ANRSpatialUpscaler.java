package com.winlator.cmod.runtime.display.renderer.effects;

/**
 * ANR Spatial reconstruction pass.
 *
 * This stage is a deterministic edge-adaptive Vulkan filter, not a neural-network
 * inference model. Temporal reconstruction is kept as a separate backend.
 */
public final class ANRSpatialUpscaler extends Effect {
    private float renderScale = 0.90f;
    private float sharpness = 0.75f;
    private int quality = 4;

    public void setRenderScale(float value) {
        renderScale = Math.max(0.25f, Math.min(1.0f, value));
    }

    public void setSharpness(float value) {
        sharpness = Math.max(0.0f, Math.min(2.0f, value));
    }

    public void setQuality(int value) {
        quality = Math.max(0, Math.min(4, value));
    }

    @Override
    public int getNativeType() {
        return TYPE_ANR_SPATIAL;
    }

    @Override
    public float[] getParams() {
        return new float[]{0f, renderScale, sharpness, quality};
    }

    @Override
    public void writeParams(float[] out, int offset) {
        out[offset] = 0f;
        out[offset + 1] = renderScale;
        out[offset + 2] = sharpness;
        out[offset + 3] = quality;
    }
}
