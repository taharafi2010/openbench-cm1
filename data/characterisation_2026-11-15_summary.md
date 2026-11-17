# Colorimeter photodiode characterisation, 15 Nov 2026

Run with `pd_characterise` v1. Conditions:
- photodiodes aligned and glued;
- gel in, cuvette slot empty, lid on;
- about 34 minutes, all 8 phases completed.

The final firmware **colorimeter_ui v2.0** is built from these results.

## Raw headline numbers

| | Sample (ADS A1) | Reference (ADS A0) |
|---|---|---|
| LED signal (empty slot) | 62.0 mV at x16 | 867 mV at x2 |
| Dark offset | 1.7–2.7 mV | 1.8–2.7 mV |
| Dark noise (SD) | 0.09–0.12 mV | 0.08–0.12 mV |
| Time to reach 90% after the LED switches on | 5.3 ms | 10.6 ms |
| Time to settle within 0.1% | 95.5 ms | 63.5 ms |
| Recommended LED settle time | 134 ms (both channels) | |
| Gain agreement | x1–x4 read about 1.5% below x8/x16 | x1 vs x2: 0.05% |
| Noise, average of 4 readings (Abs SD) | 0.0013 | 0.0017 |
| Noise, average of 16 readings (Abs SD) | 0.0009 | 0.0012 |
| Drift, 12 min of normal pulsed use | −0.48% (+0.0021 Abs) | −0.43% |
| Drift, 12 min with the LED on continuously | −0.17% | −0.16% |
| **Sample ÷ reference drift, pulsed** | **−0.045% (+0.0002 Abs)** | |

## Findings

1. **The reference works and cancels LED drift about 10×.**
   - Both channels drift together (−0.48% and −0.43%).
   - The ratio drifts only 0.045%, so the error over 12 minutes falls from 0.002 to 0.0002 Abs.
   - It does this even though the reference sees unfiltered white light while the sample sees red.
   - So v2.0 turns the reference on by default, averaged over 8 readings so it adds little noise.
2. **Settling needs about 134 ms.**
   - The electronics respond in about 5–10 ms.
   - The rest of the settle time is the LED's own output changing in its first ~100 ms, as the chip heats up.
   - v2.0 uses 150 ms by default (it was 200 ms), so readings are faster.
3. **Gain:**
   - The sample's low gains (x1–x4) read about 1.5% below x16. v2.0 corrects for this.
   - In practice the sample always runs at x16, so this only matters if the light changes a lot.
   - The reference's auto-gain step-down point was moved from 85% to 80% of the range. Its 0.87 V signal sat right on the x4/x2 boundary, so it could switch gain back and forth.
4. **The linearity test (dimming the LED with PWM) mostly measured the LED, not the electronics.**
   - The sample read up to 16% high at 3% brightness, and the reference read 6% low. If the electronics were non-linear, both channels would err the same way.
   - At low duty cycles the LED chip stays cool and gives more light per pulse. The red part of a white LED's light (from its phosphor coating) changes more with temperature than the blue.
   - So no linearity correction is applied. A blue-dye dilution series (Beer–Lambert) is the proper test of linearity.
5. **Noise:**
   - Averaging 4 → 16 readings improves precision as expected.
   - The 64-reading results came out as about 0, because of a rounding error in the test sketch's standard-deviation formula. Ignore them.
   - v2.0 keeps the average of 16: about 0.0009 Abs for the sample alone, about 0.001 with the reference on.
6. **The "DILUTE SAMPLE" warning now appears above 2.0 Abs** (it was 2.5). With a 62 mV blank, readings above about 1.5 Abs approach the noise floor.

## Raw drift data (mV, one value per minute)

```
drift_pulsed_S 62.040,61.967,61.779,61.912,61.885,61.856,61.913,61.792,61.796,61.741,61.756,61.745
drift_ledon_S  61.889,61.767,61.886,61.863,61.864,61.842,61.844,61.813,61.816,61.794,61.791,61.783
drift_pulsed_R 867.629,867.251,863.230,865.686,865.781,866.103,866.362,864.120,864.692,863.050,863.598,863.893
drift_ledon_R  865.943,864.725,865.930,865.270,865.144,865.213,865.929,865.061,864.975,864.516,864.426,864.536
```
