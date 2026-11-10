package com.winlator.cmod.feature.translator

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class OcrLineGrouperTest {
    @Test fun joinsTightlyWrappedParagraphIntoOneTranslationUnit() {
        val result = OcrLineGrouper.group(listOf(
            OcrLineGrouper.Line("APPUYEZ SUR [A] POUR VOIR LA", 100, 100, 500, 124),
            OcrLineGrouper.Line("CARTE DE CETTE ZONE", 101, 136, 390, 160)
        ))
        assertEquals(1, result.size)
        assertEquals("APPUYEZ SUR [A] POUR VOIR LA CARTE DE CETTE ZONE", result.single().text)
        assertEquals(100, result.single().top)
        assertEquals(160, result.single().bottom)
    }

    @Test fun keepsRegularlySpacedRightAlignedMenuRowsSeparate() {
        val result = OcrLineGrouper.group(listOf(
            OcrLineGrouper.Line("OPTIONS", 400, 100, 700, 128),
            OcrLineGrouper.Line("CONTROLS", 390, 158, 700, 186),
            OcrLineGrouper.Line("QUIT", 450, 216, 700, 244)
        ))
        assertEquals(3, result.size)
    }

    @Test fun joinsHyphenatedWordWithoutLeavingHyphen() {
        val result = OcrLineGrouper.group(listOf(
            OcrLineGrouper.Line("INTERNA-", 100, 100, 280, 124),
            OcrLineGrouper.Line("TIONAL", 101, 135, 260, 159)
        ))
        assertEquals(1, result.size)
        assertEquals("INTERNATIONAL", result.single().text)
    }

    @Test fun filtersButtonGlyphBesideTextButKeepsStandaloneShortWord() {
        val result = OcrLineGrouper.group(listOf(
            OcrLineGrouper.Line("A", 80, 100, 100, 124),
            OcrLineGrouper.Line("APPUYEZ POUR CONTINUER", 105, 100, 430, 124),
            OcrLineGrouper.Line("A", 600, 300, 615, 324)
        ))
        assertTrue(result.any { it.text == "APPUYEZ POUR CONTINUER" })
        assertTrue(result.any { it.text == "A" && it.top == 300 })
    }
}
