package com.winlator.cmod.feature.translator

import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/**
 * Joins tightly wrapped OCR rows into translation units while preserving regular menu rows.
 * Button glyphs are retained as protected geometry but excluded from translation text.
 */
internal object OcrLineGrouper {
    data class Line(
        val text: String,
        val left: Int,
        val top: Int,
        val right: Int,
        val bottom: Int,
        val lineCount: Int = 1
    ) {
        val width: Int get() = (right - left).coerceAtLeast(0)
        val height: Int get() = (bottom - top).coerceAtLeast(0)
    }

    private val buttonTokens = setOf("X", "Y", "L", "R", "B", "A", "LT", "RT", "□", "@", "○", "▼", "▶")

    fun cleanText(raw: String): String {
        var value = raw.replace("@", " ")
        value = value.replace(Regex("(?i)^\\s*(X|Y|L|R|B|A|LT|RT|□|○|▼|▶)(?=\\s|$)"), "")
        value = value.replace(Regex("(?i)(?<=\\s)(X|Y|L|R|B|A|LT|RT|□|○|▼|▶)\\s*$"), "")
        return value.replace(Regex("\\s+"), " ").trim()
    }

    fun isButtonToken(raw: String): Boolean {
        val token = raw.trim().uppercase().replace(" ", "")
        return token in buttonTokens || token.matches(Regex("(X|Y|L|R|B|A|LT|RT)(▼|▶)?"))
            || token.all { !it.isLetterOrDigit() }
    }

    /** Bounds for small controller glyphs adjacent to readable text; callers expand by 6–8 dp. */
    fun buttonGlyphZones(input: List<Line>): List<Line> {
        return input.filter { token ->
            if (!isButtonToken(token.text)) return@filter false
            input.any { other ->
                if (other === token || cleanText(other.text).length < 3) return@any false
                val overlap = min(token.bottom, other.bottom) - max(token.top, other.top)
                val overlapRatio = overlap.toFloat() / min(token.height, other.height).coerceAtLeast(1)
                val gap = max(0, max(token.left, other.left) - min(token.right, other.right))
                overlapRatio >= 0.30f && gap <= max(token.height, other.height) * 1.5f
            }
        }
    }

    fun group(input: List<Line>): List<Line> {
        val cleaned = input.mapNotNull { line ->
            val text = cleanText(line.text)
            if (text.isBlank() || text.none { it.isLetter() }) null else line.copy(text = text, lineCount = 1)
        }.sortedWith(compareBy<Line> { it.top }.thenBy { it.left })
        if (cleaned.isEmpty()) return emptyList()

        // Three regularly stepped rows are a menu/list, not a wrapped paragraph.
        val menuPairs = HashSet<Pair<Int, Int>>()
        for (i in 0 until cleaned.size - 2) {
            val a = cleaned[i]
            val b = cleaned[i + 1]
            val c = cleaned[i + 2]
            val h = listOf(a.height, b.height, c.height).map { it.coerceAtLeast(1) }.average()
            val step1 = b.top - a.top
            val step2 = c.top - b.top
            val regular = abs(step1 - step2) <= h * 0.20 &&
                abs(step1 - h) <= h * 0.25 &&
                abs(step2 - h) <= h * 0.25
            if (regular && sameHorizontalAnchor(a, b) && sameHorizontalAnchor(b, c)) {
                menuPairs += i to i + 1
                menuPairs += i + 1 to i + 2
            }
        }

        val result = ArrayList<Line>()
        var current = cleaned.first()
        var previous = current
        for (index in 1 until cleaned.size) {
            val next = cleaned[index]
            val previousIndex = index - 1
            val shouldJoin = (previousIndex to index) !in menuPairs && canJoin(previous, next)
            if (shouldJoin) {
                current = Line(
                    joinText(current.text, next.text),
                    min(current.left, next.left), min(current.top, next.top),
                    max(current.right, next.right), max(current.bottom, next.bottom),
                    current.lineCount + next.lineCount
                )
            } else {
                result += current
                current = next
            }
            previous = next
        }
        result += current
        return result
    }

    /** Human-readable reason strings for the optional debug log. */
    fun debugDecisions(input: List<Line>): List<String> {
        val rows = input.map { it.copy(text = cleanText(it.text)) }
            .filter { it.text.isNotBlank() && it.text.any(Char::isLetter) }
            .sortedWith(compareBy<Line> { it.top }.thenBy { it.left })
        return rows.zipWithNext().map { (a, b) -> "${a.text.take(24)} -> ${b.text.take(24)}: ${decision(a, b)}" }
    }

    private fun decision(a: Line, b: Line): String {
        val ah = a.height.coerceAtLeast(1)
        val bh = b.height.coerceAtLeast(1)
        val gap = b.top - a.bottom
        if (gap < -0.15f * min(ah, bh)) return "keep: overlapping OCR boxes"
        if (gap >= 0.60f * min(ah, bh)) return "keep: vertical gap >= 0.6 line"
        if (abs(ah - bh) > 0.25f * max(ah, bh)) return "keep: line heights differ >25%"
        if (!sameHorizontalAnchor(a, b)) return "keep: no common left/right/center anchor"
        if (abs(a.right - b.right) <= max(3f, max(a.width, b.width) * 0.10f) &&
            gap >= 0.35f * min(ah, bh)) return "keep: right-aligned menu-row heuristic"
        return "join: tight leading and shared alignment"
    }

    private fun canJoin(a: Line, b: Line): Boolean = decision(a, b).startsWith("join:")

    private fun sameHorizontalAnchor(a: Line, b: Line): Boolean {
        val tolerance = max(3f, max(a.width, b.width) * 0.10f)
        val sameLeft = abs(a.left - b.left) <= tolerance
        val sameRight = abs(a.right - b.right) <= tolerance
        val sameCenter = abs((a.left + a.right) - (b.left + b.right)) * 0.5f <= tolerance
        return sameLeft || sameRight || sameCenter
    }

    private fun joinText(first: String, second: String): String {
        val a = first.trimEnd()
        val b = second.trimStart()
        return if (a.endsWith("-") || a.endsWith("‐") || a.endsWith("‑")) {
            a.dropLast(1) + b
        } else "$a $b"
    }
}
