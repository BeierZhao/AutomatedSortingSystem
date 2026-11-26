package ca.mcmaster.sorting.vision

import org.junit.Assert.assertEquals
import org.junit.Test

/** Boxes laid out like the reference label: key column at x 0..200, value column at x 220..400. */
class LabelParserTest {
    private val parser = LabelParser()

    private fun key(text: String, row: Int) = TextBox(text, 0, row * 40, 200, row * 40 + 30)
    private fun value(text: String, row: Int) = TextBox(text, 220, row * 40 + 2, 400, row * 40 + 30)
    private fun line(text: String, row: Int) = TextBox(text, 0, row * 40, 400, row * 40 + 30)
    private fun barcode(raw: String?) = BarcodeBox(raw, 20, 150, 380, 260)
    private val digits = TextBox("345678", 150, 265, 250, 285)

    @Test fun separateCells() {
        val r = parser.parse(
            listOf(key("Origin:", 0), value("Hamilton", 0), key("Destination:", 1), value("Toronto", 1),
                key("Part ID:", 2), digits),
            listOf(barcode("345678")),
        )
        assertEquals(LabelReading("success", "Hamilton", "Toronto", "345678"), r)
    }

    @Test fun mergedLinesAndOcrNoise() {
        val r = parser.parse(
            listOf(line("0rigin: Oakvi1le", 0), line("Destlnation Hamilton", 1), line("Part 1D:", 2)),
            listOf(barcode("345678")),
        )
        assertEquals(LabelReading("success", "Oakville", "Hamilton", "345678"), r)
    }

    @Test fun misspelledCitySnapsToKnownName() {
        val r = parser.parse(
            listOf(key("Origin:", 0), value("Missisauga", 0), key("Destination:", 1), value("Toronto", 1)),
            listOf(barcode("223456")),
        )
        assertEquals(LabelReading("success", "Mississauga", "Toronto", "223456"), r)
    }

    @Test fun snapDisabledKeepsOcrText() {
        val r = LabelParser(snapToKnownCities = false).parse(
            listOf(line("Origin: Missisauga", 0), line("Destination: Toronto", 1)),
            listOf(barcode("223456")),
        )
        assertEquals("Missisauga", r.origin)
    }

    @Test fun unknownCityIsReturnedAsRead() {
        val r = parser.parse(listOf(line("Origin: Kingston", 0), line("Destination: Toronto", 1)), listOf(barcode("1")))
        assertEquals(LabelReading("success", "Kingston", "Toronto", "1"), r)
    }

    @Test fun blankDestinationIsNone() {
        val r = parser.parse(
            listOf(key("Origin:", 0), value("Hamilton", 0), key("Destination:", 1), key("Part ID:", 2)),
            listOf(barcode("345678")),
        )
        assertEquals(LabelReading("info lost", "Hamilton", "none", "345678"), r)
    }

    @Test fun valueOnAnotherRowIsNotTaken() {
        // "Toronto" belongs to row 1; row 0 has no value.
        val r = parser.parse(
            listOf(key("Origin:", 0), key("Destination:", 1), value("Toronto", 1)),
            listOf(barcode("5")),
        )
        assertEquals("none", r.origin)
        assertEquals("Toronto", r.destination)
    }

    @Test fun missingOriginLabelIsFault() {
        val r = parser.parse(listOf(line("Destination: Toronto", 1)), listOf(barcode("345678")))
        assertEquals(LabelReading("info lost", "fault", "Toronto", "345678"), r)
    }

    @Test fun partIdKeyWithoutBarcodeIsNone() {
        val r = parser.parse(
            listOf(line("Origin: Hamilton", 0), line("Destination: Toronto", 1), line("Part ID:", 2)),
            emptyList(),
        )
        assertEquals(LabelReading("info lost", "Hamilton", "Toronto", "none"), r)
    }

    @Test fun noPartIdAtAllIsFault() {
        val r = parser.parse(listOf(line("Origin: Hamilton", 0), line("Destination: Toronto", 1)), emptyList())
        assertEquals(LabelReading("info lost", "Hamilton", "Toronto", "fault"), r)
    }

    @Test fun nonIntegerOrOutOfRangeBarcodeIsNone() {
        val labels = listOf(line("Origin: Hamilton", 0), line("Destination: Toronto", 1))
        assertEquals("none", parser.parse(labels, listOf(barcode("ABC-12"))).partId)
        assertEquals("none", parser.parse(labels, listOf(barcode("2147483648"))).partId)
        assertEquals("2147483647", parser.parse(labels, listOf(barcode("2147483647"))).partId)
        assertEquals("12345", parser.parse(labels, listOf(barcode("012345"))).partId)
    }

    @Test fun nothingFoundIsNone() {
        val r = parser.parse(listOf(line("random text", 0)), emptyList())
        assertEquals(LabelReading("none", "fault", "fault", "fault"), r)
    }

    @Test fun jsonUsesSpecFieldNames() {
        val json = LabelReading("success", "Hamilton", "Toronto", "345678").toJson()
        assertEquals("success", json.getString("reading_result"))
        assertEquals("Hamilton", json.getString("origin"))
        assertEquals("Toronto", json.getString("destination"))
        assertEquals("345678", json.getString("part_id"))
    }
}
