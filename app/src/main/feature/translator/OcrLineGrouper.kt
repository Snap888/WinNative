package com.winlator.cmod.feature.translator

import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/**
 * Pure geometry/text stage between ML Kit and translation. It deliberately keeps menu rows
 * separate: a paragraph needs tight vertical leading, similar line heights and a shared edge.
 */
internal object OcrLineGrouper {
    data class Line(val text: String, val left: Int, val top: Int, val right: Int, val bottom: Int) {
        val width: Int get() = (right - left).coerceAtLeast(0)
        val height: Int get() = (bottom - top).coerceAtLeast(0)
    }

    fun group(input: List<Line>): List<Line> {
        val lines = removeButtonArtifacts(input).sortedWith(compareBy<Line> { it.top }.thenBy { it.left })
        if (lines.isEmpty()) return emptyList()
        val result = ArrayList<Line>()
        var current = lines.first()
        for (next in lines.drop(1)) {
            if (canJoin(current, next)) {
                current = Line(
                    joinText(current.text, next.text),
                    min(current.left, next.left), min(current.top, next.top),
                    max(current.right, next.right), max(current.bottom, next.bottom)
                )
            } else {
                result += current
                current = next
            }
        }
        result += current
        return result
    }

    private fun canJoin(a: Line, b: Line): Boolean {
        val ah = a.height.coerceAtLeast(1)
        val bh = b.height.coerceAtLeast(1)
        val gap = b.top - a.bottom
        if (gap < -0.15f * min(ah, bh) || gap >= 0.60f * min(ah, bh)) return false
        if (abs(ah - bh) > 0.25f * max(ah, bh)) return false

        val width = max(a.width, b.width).coerceAtLeast(1)
        val tolerance = max(3f, width * 0.10f)
        val sameLeft = abs(a.left - b.left) <= tolerance
        val sameRight = abs(a.right - b.right) <= tolerance
        val sameCenter = abs((a.left + a.right) - (b.left + b.right)) * 0.5f <= tolerance
        if (!(sameLeft || sameRight || sameCenter)) return false

        // Right-aligned menu labels commonly form a vertical list. Keep regular rows separate.
        if (sameRight && gap >= 0.35f * min(ah, bh) && a.width > 0 && b.width > 0) return false
        return true
    }

    private fun removeButtonArtifacts(input: List<Line>): List<Line> {
        val symbols = setOf("□", "■", "▢", "▣", "@", "○", "●", "◯", "◇", "◆")
        return input.filterNot { token ->
            val value = token.text.trim()
            if (value.isEmpty()) return@filterNot true
            if (value in symbols) return@filterNot true
            if (value.length !in 1..2) return@filterNot false
            val buttonLetter = value in setOf("A", "B", "X", "Y", "Б", "А", "В")
            val nonWordGlyph = value.none { it.isLetterOrDigit() }
            if (!buttonLetter && !nonWordGlyph) return@filterNot false
            input.any { other ->
                if (other === token || other.text.trim().length < 3) return@any false
                val verticalOverlap = min(token.bottom, other.bottom) - max(token.top, other.top)
                val overlapRatio = verticalOverlap.toFloat() / min(token.height, other.height).coerceAtLeast(1)
                val horizontalGap = max(0, max(token.left, other.left) - min(token.right, other.right))
                overlapRatio >= 0.35f && horizontalGap <= max(token.height, other.height) * 1.2f
            }
        }
    }

    private fun joinText(first: String, second: String): String {
        val a = first.trimEnd()
        val b = second.trimStart()
        return if (a.endsWith("-") || a.endsWith("‐") || a.endsWith("‑")) {
            a.dropLast(1) + b
        } else "$a $b"
    }
}
