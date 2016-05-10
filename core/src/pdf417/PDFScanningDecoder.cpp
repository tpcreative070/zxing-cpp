/*
* Copyright 2016 ZXing authors
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "pdf417/PDFScanningDecoder.h"
#include "pdf417/PDFBoundingBox.h"
#include "pdf417/PDFDetectionResultColumn.h"
#include "pdf417/PDFCommon.h"
#include "pdf417/PDFCodewordDecoder.h"
#include "pdf417/PDFBarcodeMetadata.h"
#include "pdf417/PDFDetectionResult.h"
#include "pdf417/PDFBarcodeValue.h"
#include "pdf417/PDFErrorCorrection.h"
#include "pdf417/PDFDecodedBitStreamParser.h"
#include "ResultPoint.h"
#include "ZXNullable.h"
#include "BitMatrix.h"
#include "DecoderResult.h"
#include "ErrorStatus.h"
#include "DecoderResult.h"

#include <numeric>
#include <array>

namespace ZXing {
namespace Pdf417 {

static const int CODEWORD_SKEW_SIZE = 2;
static const int MAX_ERRORS = 3;
static const int MAX_EC_CODEWORDS = 512;

typedef std::array<int, Common::BARS_IN_MODULE> ModuleBitCountType;

static int AdjustCodewordStartColumn(const BitMatrix& image, int minColumn, int maxColumn, bool leftToRight, int codewordStartColumn, int imageRow)
{
	int correctedStartColumn = codewordStartColumn;
	int increment = leftToRight ? -1 : 1;
	// there should be no black pixels before the start column. If there are, then we need to start earlier.
	for (int i = 0; i < 2; i++) {
		while ((leftToRight ? correctedStartColumn >= minColumn : correctedStartColumn < maxColumn) &&
			leftToRight == image.get(correctedStartColumn, imageRow)) {
			if (std::abs(codewordStartColumn - correctedStartColumn) > CODEWORD_SKEW_SIZE) {
				return codewordStartColumn;
			}
			correctedStartColumn += increment;
		}
		increment = -increment;
		leftToRight = !leftToRight;
	}
	return correctedStartColumn;
}

static bool GetModuleBitCount(const BitMatrix& image, int minColumn, int maxColumn, bool leftToRight, int startColumn, int imageRow, ModuleBitCountType& moduleBitCount)
{
	int imageColumn = startColumn;
	size_t moduleNumber = 0;
	int increment = leftToRight ? 1 : -1;
	bool previousPixelValue = leftToRight;
	std::fill(moduleBitCount.begin(), moduleBitCount.end(), 0);
	while ((leftToRight ? (imageColumn < maxColumn) : (imageColumn >= minColumn)) && moduleNumber < moduleBitCount.size()) {
		if (image.get(imageColumn, imageRow) == previousPixelValue) {
			moduleBitCount[moduleNumber] += 1;
			imageColumn += increment;
		}
		else {
			moduleNumber += 1;
			previousPixelValue = !previousPixelValue;
		}
	}
	return moduleNumber == moduleBitCount.size() || (imageColumn == (leftToRight ? maxColumn : minColumn) && moduleNumber == moduleBitCount.size() - 1);
}

static bool CheckCodewordSkew(int codewordSize, int minCodewordWidth, int maxCodewordWidth)
{
	return minCodewordWidth - CODEWORD_SKEW_SIZE <= codewordSize &&
		codewordSize <= maxCodewordWidth + CODEWORD_SKEW_SIZE;
}


static void GetBitCountForCodeword(int codeword, ModuleBitCountType& result)
{
	std::fill(result.begin(), result.end(), 0);
	int previousValue = 0;
	int i = static_cast<int>(result.size()) - 1;
	while (true) {
		if ((codeword & 0x1) != previousValue) {
			previousValue = codeword & 0x1;
			i--;
			if (i < 0) {
				break;
			}
		}
		result[i]++;
		codeword >>= 1;
	}
}

static int GetCodewordBucketNumber(const ModuleBitCountType& moduleBitCount)
{
	return (moduleBitCount[0] - moduleBitCount[2] + moduleBitCount[4] - moduleBitCount[6] + 9) % 9;
}

static int GetCodewordBucketNumber(int codeword)
{
	ModuleBitCountType bitcount = {};
	GetBitCountForCodeword(codeword, bitcount);
	return GetCodewordBucketNumber(bitcount);
}

static Nullable<Codeword> DetectCodeword(const BitMatrix& image, int minColumn, int maxColumn, bool leftToRight, int startColumn, int imageRow, int minCodewordWidth, int maxCodewordWidth)
{
	startColumn = AdjustCodewordStartColumn(image, minColumn, maxColumn, leftToRight, startColumn, imageRow);
	// we usually know fairly exact now how long a codeword is. We should provide minimum and maximum expected length
	// and try to adjust the read pixels, e.g. remove single pixel errors or try to cut off exceeding pixels.
	// min and maxCodewordWidth should not be used as they are calculated for the whole barcode an can be inaccurate
	// for the current position
	ModuleBitCountType moduleBitCount;
	if (!GetModuleBitCount(image, minColumn, maxColumn, leftToRight, startColumn, imageRow, moduleBitCount)) {
		return nullptr;
	}
	int endColumn;
	int codewordBitCount = std::accumulate(moduleBitCount.begin(), moduleBitCount.end(), 0);
	if (leftToRight) {
		endColumn = startColumn + codewordBitCount;
	}
	else {
		std::reverse(moduleBitCount.begin(), moduleBitCount.end());
		endColumn = startColumn;
		startColumn = endColumn - codewordBitCount;
	}
	// TODO implement check for width and correction of black and white bars
	// use start (and maybe stop pattern) to determine if blackbars are wider than white bars. If so, adjust.
	// should probably done only for codewords with a lot more than 17 bits. 
	// The following fixes 10-1.png, which has wide black bars and small white bars
	//    for (int i = 0; i < moduleBitCount.length; i++) {
	//      if (i % 2 == 0) {
	//        moduleBitCount[i]--;
	//      } else {
	//        moduleBitCount[i]++;
	//      }
	//    }

	// We could also use the width of surrounding codewords for more accurate results, but this seems
	// sufficient for now
	if (!CheckCodewordSkew(codewordBitCount, minCodewordWidth, maxCodewordWidth)) {
		// We could try to use the startX and endX position of the codeword in the same column in the previous row,
		// create the bit count from it and normalize it to 8. This would help with single pixel errors.
		return nullptr;
	}

	int decodedValue = CodewordDecoder::GetDecodedValue(moduleBitCount);
	if (decodedValue != -1) {
		int codeword = Common::GetCodeword(decodedValue);
		if (codeword != -1) {
			return Codeword(startColumn, endColumn, GetCodewordBucketNumber(decodedValue), codeword);
		}
	}
	return nullptr;
}

static DetectionResultColumn GetRowIndicatorColumn(const BitMatrix& image, const BoundingBox& boundingBox, const ResultPoint& startPoint, bool leftToRight, int minCodewordWidth, int maxCodewordWidth)
{
	DetectionResultColumn rowIndicatorColumn(boundingBox, leftToRight ? DetectionResultColumn::RowIndicator::Left : DetectionResultColumn::RowIndicator::Right);
	for (int i = 0; i < 2; i++) {
		int increment = i == 0 ? 1 : -1;
		int startColumn = (int)startPoint.x();
		for (int imageRow = (int)startPoint.y(); imageRow <= boundingBox.maxY() && imageRow >= boundingBox.minY(); imageRow += increment) {
			auto codeword = DetectCodeword(image, 0, image.width(), leftToRight, startColumn, imageRow, minCodewordWidth, maxCodewordWidth);
			if (codeword != nullptr) {
				rowIndicatorColumn.setCodeword(imageRow, codeword);
				if (leftToRight) {
					startColumn = codeword.value().startX();
				}
				else {
					startColumn = codeword.value().endX();
				}
			}
		}
	}
	return rowIndicatorColumn;
}

static bool GetBarcodeMetadata(Nullable<DetectionResultColumn>& leftRowIndicatorColumn, Nullable<DetectionResultColumn>& rightRowIndicatorColumn, BarcodeMetadata& result)
{
	BarcodeMetadata leftBarcodeMetadata;
	if (leftRowIndicatorColumn == nullptr || !leftRowIndicatorColumn.value().getBarcodeMetadata(leftBarcodeMetadata)) {
		return rightRowIndicatorColumn != nullptr && rightRowIndicatorColumn.value().getBarcodeMetadata(result);
	}

	BarcodeMetadata rightBarcodeMetadata;
	if (rightRowIndicatorColumn == nullptr || !rightRowIndicatorColumn.value().getBarcodeMetadata(rightBarcodeMetadata)) {
		result = leftBarcodeMetadata;
		return true;
	}

	if (leftBarcodeMetadata.columnCount() != rightBarcodeMetadata.columnCount() &&
		leftBarcodeMetadata.errorCorrectionLevel() != rightBarcodeMetadata.errorCorrectionLevel() &&
		leftBarcodeMetadata.rowCount() != rightBarcodeMetadata.rowCount()) {
		return false;
	}
	result = leftBarcodeMetadata;
	return true;
}

template <typename Iter>
static auto GetMax(Iter start, Iter end) -> typename std::remove_reference<decltype(*start)>::type
{
	auto it = std::max_element(start, end);
	return it != end ? *it : -1;
}

static bool AdjustBoundingBox(Nullable<DetectionResultColumn>& rowIndicatorColumn, Nullable<BoundingBox>& result)
{
	if (rowIndicatorColumn == nullptr) {
		result = nullptr;
		return true;
	}
	std::vector<int> rowHeights;
	if (!rowIndicatorColumn.value().getRowHeights(rowHeights)) {
		result = nullptr;
		return true;
	}
	int maxRowHeight = GetMax(rowHeights.begin(), rowHeights.end());
	int missingStartRows = 0;
	for (int rowHeight : rowHeights) {
		missingStartRows += maxRowHeight - rowHeight;
		if (rowHeight > 0) {
			break;
		}
	}
	auto& codewords = rowIndicatorColumn.value().allCodewords();
	for (int row = 0; missingStartRows > 0 && codewords[row] == nullptr; row++) {
		missingStartRows--;
	}
	int missingEndRows = 0;
	for (int row = (int)rowHeights.size() - 1; row >= 0; row--) {
		missingEndRows += maxRowHeight - rowHeights[row];
		if (rowHeights[row] > 0) {
			break;
		}
	}
	for (int row = (int)codewords.size() - 1; missingEndRows > 0 && codewords[row] == nullptr; row--) {
		missingEndRows--;
	}
	BoundingBox box;
	if (BoundingBox::AddMissingRows(rowIndicatorColumn.value().boundingBox(), missingStartRows, missingEndRows, rowIndicatorColumn.value().isLeftRowIndicator(), box)) {
		result = box;
		return true;
	}
	return false;
}

static bool Merge(Nullable<DetectionResultColumn>& leftRowIndicatorColumn, Nullable<DetectionResultColumn>& rightRowIndicatorColumn, DetectionResult& result)
{
	if (leftRowIndicatorColumn != nullptr || rightRowIndicatorColumn != nullptr) {
		BarcodeMetadata barcodeMetadata;
		if (GetBarcodeMetadata(leftRowIndicatorColumn, rightRowIndicatorColumn, barcodeMetadata)) {
			Nullable<BoundingBox> leftBox, rightBox, mergedBox;
			if (AdjustBoundingBox(leftRowIndicatorColumn, leftBox) && AdjustBoundingBox(rightRowIndicatorColumn, rightBox) && BoundingBox::Merge(leftBox, rightBox, mergedBox)) {
				result.init(barcodeMetadata, mergedBox);
				return true;
			}
		}
	}
	return false;
}

static bool IsValidBarcodeColumn(const DetectionResult& detectionResult, int barcodeColumn)
{
	return barcodeColumn >= 0 && barcodeColumn <= detectionResult.barcodeColumnCount() + 1;
}

static int GetStartColumn(const DetectionResult& detectionResult, int barcodeColumn, int imageRow, bool leftToRight)
{
	int offset = leftToRight ? 1 : -1;
	Nullable<Codeword> codeword;
	if (IsValidBarcodeColumn(detectionResult, barcodeColumn - offset)) {
		codeword = detectionResult.column(barcodeColumn - offset).value().codeword(imageRow);
	}
	if (codeword != nullptr) {
		return leftToRight ? codeword.value().endX() : codeword.value().startX();
	}
	codeword = detectionResult.column(barcodeColumn).value().codewordNearby(imageRow);
	if (codeword != nullptr) {
		return leftToRight ? codeword.value().startX() : codeword.value().endX();
	}
	if (IsValidBarcodeColumn(detectionResult, barcodeColumn - offset)) {
		codeword = detectionResult.column(barcodeColumn - offset).value().codewordNearby(imageRow);
	}
	if (codeword != nullptr) {
		return leftToRight ? codeword.value().endX() : codeword.value().startX();
	}
	int skippedColumns = 0;

	while (IsValidBarcodeColumn(detectionResult, barcodeColumn - offset)) {
		barcodeColumn -= offset;
		for (auto& previousRowCodeword : detectionResult.column(barcodeColumn).value().allCodewords()) {
			if (previousRowCodeword != nullptr) {
				return (leftToRight ? previousRowCodeword.value().endX() : previousRowCodeword.value().startX()) +
					offset *
					skippedColumns *
					(previousRowCodeword.value().endX() - previousRowCodeword.value().startX());
			}
		}
		skippedColumns++;
	}
	return leftToRight ? detectionResult.getBoundingBox().value().minX() : detectionResult.getBoundingBox().value().maxX();
}

static void CreateBarcodeMatrix(DetectionResult& detectionResult, std::vector<std::vector<BarcodeValue>>& barcodeMatrix)
{
	barcodeMatrix.resize(detectionResult.barcodeRowCount());
	for (auto& row : barcodeMatrix) {
		row.resize(detectionResult.barcodeColumnCount() + 2);
	}

	int column = 0;
	for (auto& resultColumn : detectionResult.allColumns()) {
		if (resultColumn != nullptr) {
			for (auto& codeword : resultColumn.value().allCodewords()) {
				if (codeword != nullptr) {
					int rowNumber = codeword.value().rowNumber();
					if (rowNumber >= 0) {
						if (rowNumber >= (int)barcodeMatrix.size()) {
							// We have more rows than the barcode metadata allows for, ignore them.
							continue;
						}
						barcodeMatrix[rowNumber][column].setValue(codeword.value().value());
					}
				}
			}
		}
		column++;
	}
}

static int GetNumberOfECCodeWords(int barcodeECLevel)
{
	return 2 << barcodeECLevel;
}

static bool AdjustCodewordCount(const DetectionResult& detectionResult, std::vector<std::vector<BarcodeValue>>& barcodeMatrix)
{
	auto numberOfCodewords = barcodeMatrix[0][1].value();
	int calculatedNumberOfCodewords = detectionResult.barcodeColumnCount() * detectionResult.barcodeRowCount() - GetNumberOfECCodeWords(detectionResult.barcodeECLevel());
	if (numberOfCodewords.empty()) {
		if (calculatedNumberOfCodewords < 1 || calculatedNumberOfCodewords > Common::MAX_CODEWORDS_IN_BARCODE) {
			return false;
		}
		barcodeMatrix[0][1].setValue(calculatedNumberOfCodewords);
	}
	else if (numberOfCodewords[0] != calculatedNumberOfCodewords) {
		// The calculated one is more reliable as it is derived from the row indicator columns
		barcodeMatrix[0][1].setValue(calculatedNumberOfCodewords);
	}
	return true;
}


/**
* <p>Given data and error-correction codewords received, possibly corrupted by errors, attempts to
* correct the errors in-place.</p>
*
* @param codewords   data and error correction codewords
* @param erasures positions of any known erasures
* @param numECCodewords number of error correction codewords that are available in codewords
* @throws ChecksumException if error correction fails
*/
static ErrorStatus CorrectErrors(std::vector<int>& codewords, const std::vector<int>& erasures, int numECCodewords, int& errorCount)
{
	if ((int)erasures.size() > numECCodewords / 2 + MAX_ERRORS ||
		numECCodewords < 0 ||
		numECCodewords > MAX_EC_CODEWORDS) {
		// Too many errors or EC Codewords is corrupted
		return ErrorStatus::ChecksumError;
	}
	return ErrorCorrection::Decode(codewords, numECCodewords, erasures, errorCount) ? ErrorStatus::NoError : ErrorStatus::ChecksumError;
}

/**
* Verify that all is OK with the codeword array.
*/
static ErrorStatus VerifyCodewordCount(std::vector<int>& codewords, int numECCodewords)
{
	if (codewords.size() < 4) {
		// Codeword array size should be at least 4 allowing for
		// Count CW, At least one Data CW, Error Correction CW, Error Correction CW
		return ErrorStatus::FormatError;
	}
	// The first codeword, the Symbol Length Descriptor, shall always encode the total number of data
	// codewords in the symbol, including the Symbol Length Descriptor itself, data codewords and pad
	// codewords, but excluding the number of error correction codewords.
	int numberOfCodewords = codewords[0];
	if (numberOfCodewords > (int)codewords.size()) {
		return ErrorStatus::FormatError;
	}
	if (numberOfCodewords == 0) {
		// Reset to the length of the array - 8 (Allow for at least level 3 Error Correction (8 Error Codewords)
		if (numECCodewords < (int)codewords.size()) {
			codewords[0] = (int)codewords.size() - numECCodewords;
		}
		else {
			return ErrorStatus::FormatError;
		}
	}
	return ErrorStatus::NoError;
}

static ErrorStatus DecodeCodewords(std::vector<int>& codewords, int ecLevel, const std::vector<int>& erasures, const StringCodecs& codec, DecoderResult& result)
{
	if (codewords.empty()) {
		return ErrorStatus::FormatError;
	}

	int numECCodewords = 1 << (ecLevel + 1);
	int correctedErrorsCount = 0;
	ErrorStatus status = CorrectErrors(codewords, erasures, numECCodewords, correctedErrorsCount);
	if (StatusIsOK(status)) {
		status = VerifyCodewordCount(codewords, numECCodewords);
		if (StatusIsOK(status)) {
			// Decode the codewords
			status = DecodedBitStreamParser::Decode(codewords, ecLevel, codec, result);
			if (StatusIsOK(status)) {
				result.setErrorsCorrected(correctedErrorsCount);
				result.setErasures(erasures.size());
				return ErrorStatus::NoError;
			}
		}
	}
	return status;
}


/**
* This method deals with the fact, that the decoding process doesn't always yield a single most likely value. The
* current error correction implementation doesn't deal with erasures very well, so it's better to provide a value
* for these ambiguous codewords instead of treating it as an erasure. The problem is that we don't know which of
* the ambiguous values to choose. We try decode using the first value, and if that fails, we use another of the
* ambiguous values and try to decode again. This usually only happens on very hard to read and decode barcodes,
* so decoding the normal barcodes is not affected by this.
*
* @param erasureArray contains the indexes of erasures
* @param ambiguousIndexes array with the indexes that have more than one most likely value
* @param ambiguousIndexValues two dimensional array that contains the ambiguous values. The first dimension must
* be the same length as the ambiguousIndexes array
*/
static ErrorStatus CreateDecoderResultFromAmbiguousValues(int ecLevel, std::vector<int>& codewords, const std::vector<int>& erasureArray, const std::vector<int>& ambiguousIndexes, const std::vector<std::vector<int>>& ambiguousIndexValues, const StringCodecs& codec, DecoderResult& result)
{
	std::vector<int> ambiguousIndexCount(ambiguousIndexes.size(), 0);

	int tries = 100;
	while (tries-- > 0) {
		for (size_t i = 0; i < ambiguousIndexCount.size(); i++) {
			codewords[ambiguousIndexes[i]] = ambiguousIndexValues[i][ambiguousIndexCount[i]];
		}
		auto status = DecodeCodewords(codewords, ecLevel, erasureArray, codec, result);
		if (status != ErrorStatus::ChecksumError) {
			return status;
		}

		if (ambiguousIndexCount.empty()) {
			return ErrorStatus::ChecksumError;
		}
		for (size_t i = 0; i < ambiguousIndexCount.size(); i++) {
			if (ambiguousIndexCount[i] < (int)ambiguousIndexValues[i].size() - 1) {
				ambiguousIndexCount[i]++;
				break;
			}
			else {
				ambiguousIndexCount[i] = 0;
				if (i == ambiguousIndexCount.size() - 1) {
					return ErrorStatus::ChecksumError;
				}
			}
		}
	}
	return ErrorStatus::ChecksumError;
}


static ErrorStatus CreateDecoderResult(DetectionResult& detectionResult, const StringCodecs& codec, DecoderResult& result)
{
	std::vector<std::vector<BarcodeValue>> barcodeMatrix;
	CreateBarcodeMatrix(detectionResult, barcodeMatrix);
	if (!AdjustCodewordCount(detectionResult, barcodeMatrix)) {
		return ErrorStatus::NotFound;
	}
	std::vector<int> erasures;
	std::vector<int> codewords(detectionResult.barcodeRowCount() * detectionResult.barcodeColumnCount(), 0);
	std::vector<std::vector<int>> ambiguousIndexValues;
	std::vector<int> ambiguousIndexesList;
	for (int row = 0; row < detectionResult.barcodeRowCount(); row++) {
		for (int column = 0; column < detectionResult.barcodeColumnCount(); column++) {
			auto values = barcodeMatrix[row][column + 1].value();
			int codewordIndex = row * detectionResult.barcodeColumnCount() + column;
			if (values.empty()) {
				erasures.push_back(codewordIndex);
			}
			else if (values.size() == 1) {
				codewords[codewordIndex] = values[0];
			}
			else {
				ambiguousIndexesList.push_back(codewordIndex);
				ambiguousIndexValues.push_back(values);
			}
		}
	}
	return CreateDecoderResultFromAmbiguousValues(detectionResult.barcodeECLevel(), codewords, erasures, ambiguousIndexesList, ambiguousIndexValues, codec, result);
}


// TODO don't pass in minCodewordWidth and maxCodewordWidth, pass in barcode columns for start and stop pattern
// columns. That way width can be deducted from the pattern column.
// This approach also allows to detect more details about the barcode, e.g. if a bar type (white or black) is wider 
// than it should be. This can happen if the scanner used a bad blackpoint.
ErrorStatus
ScanningDecoder::Decode(const BitMatrix& image, const Nullable<ResultPoint>& imageTopLeft, const Nullable<ResultPoint>& imageBottomLeft,
	const Nullable<ResultPoint>& imageTopRight, const Nullable<ResultPoint>& imageBottomRight,
	int minCodewordWidth, int maxCodewordWidth, const StringCodecs& codec, DecoderResult& result)
{
	BoundingBox boundingBox;
	if (!BoundingBox::Create(image.width(), image.height(), imageTopLeft, imageBottomLeft, imageTopRight, imageBottomRight, boundingBox)) {
		return ErrorStatus::NotFound;
	}
	
	Nullable<DetectionResultColumn> leftRowIndicatorColumn, rightRowIndicatorColumn;
	DetectionResult detectionResult;
	for (int i = 0; i < 2; i++) {
		if (imageTopLeft != nullptr) {
			leftRowIndicatorColumn = GetRowIndicatorColumn(image, boundingBox, imageTopLeft, true, minCodewordWidth, maxCodewordWidth);
		}
		if (imageTopRight != nullptr) {
			rightRowIndicatorColumn = GetRowIndicatorColumn(image, boundingBox, imageTopRight, false, minCodewordWidth, maxCodewordWidth);
		}
		if (!Merge(leftRowIndicatorColumn, rightRowIndicatorColumn, detectionResult)) {
			return ErrorStatus::NotFound;
		}
		if (i == 0 && detectionResult.getBoundingBox() != nullptr && (detectionResult.getBoundingBox().value().minY() < boundingBox.minY() || detectionResult.getBoundingBox().value().maxY() > boundingBox.maxY())) {
			boundingBox = detectionResult.getBoundingBox();
		}
		else {
			detectionResult.setBoundingBox(boundingBox);
			break;
		}
	}

	int maxBarcodeColumn = detectionResult.barcodeColumnCount() + 1;
	detectionResult.setColumn(0, leftRowIndicatorColumn);
	detectionResult.setColumn(maxBarcodeColumn, rightRowIndicatorColumn);

	bool leftToRight = leftRowIndicatorColumn != nullptr;
	for (int barcodeColumnCount = 1; barcodeColumnCount <= maxBarcodeColumn; barcodeColumnCount++) {
		int barcodeColumn = leftToRight ? barcodeColumnCount : maxBarcodeColumn - barcodeColumnCount;
		if (detectionResult.column(barcodeColumn) != nullptr) {
			// This will be the case for the opposite row indicator column, which doesn't need to be decoded again.
			continue;
		}
		DetectionResultColumn::RowIndicator rowIndicator = barcodeColumn == 0 ? DetectionResultColumn::RowIndicator::Left : (barcodeColumn == maxBarcodeColumn ? DetectionResultColumn::RowIndicator::Right : DetectionResultColumn::RowIndicator::None);
		detectionResult.setColumn(barcodeColumn, DetectionResultColumn(boundingBox, rowIndicator));
		int startColumn = -1;
		int previousStartColumn = startColumn;
		// TODO start at a row for which we know the start position, then detect upwards and downwards from there.
		for (int imageRow = boundingBox.minY(); imageRow <= boundingBox.maxY(); imageRow++) {
			startColumn = GetStartColumn(detectionResult, barcodeColumn, imageRow, leftToRight);
			if (startColumn < 0 || startColumn > boundingBox.maxX()) {
				if (previousStartColumn == -1) {
					continue;
				}
				startColumn = previousStartColumn;
			}
			Nullable<Codeword> codeword = DetectCodeword(image, boundingBox.minX(), boundingBox.maxX(), leftToRight, startColumn, imageRow, minCodewordWidth, maxCodewordWidth);
			if (codeword != nullptr) {
				detectionResult.column(barcodeColumn).value().setCodeword(imageRow, codeword);
				previousStartColumn = startColumn;
				minCodewordWidth = std::min(minCodewordWidth, codeword.value().width());
				maxCodewordWidth = std::max(maxCodewordWidth, codeword.value().width());
			}
		}
	}
	return CreateDecoderResult(detectionResult, codec, result);
}

//
//public static String toString(BarcodeValue[][] barcodeMatrix) {
//	Formatter formatter = new Formatter();
//	for (int row = 0; row < barcodeMatrix.length; row++) {
//		formatter.format("Row %2d: ", row);
//		for (int column = 0; column < barcodeMatrix[row].length; column++) {
//			BarcodeValue barcodeValue = barcodeMatrix[row][column];
//			if (barcodeValue.getValue().length == 0) {
//				formatter.format("        ", (Object[]) null);
//			}
//			else {
//				formatter.format("%4d(%2d)", barcodeValue.getValue()[0],
//					barcodeValue.getConfidence(barcodeValue.getValue()[0]));
//			}
//		}
//		formatter.format("%n");
//	}
//	String result = formatter.toString();
//	formatter.close();
//	return result;
//}

} // Pdf417
} // ZXing