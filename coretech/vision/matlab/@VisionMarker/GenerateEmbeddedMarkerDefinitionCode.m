function outputString = GenerateEmbeddedMarkerDefinitionCode(varargin)

numFractionalBits = 15;

parseVarargin(varargin{:});
 
maxX = max(max(abs(VisionMarker.XProbes - double(int16(round(2^numFractionalBits)*VisionMarker.XProbes))/(2^numFractionalBits))));
maxY = max(max(abs(VisionMarker.YProbes - double(int16(round(2^numFractionalBits)*VisionMarker.YProbes))/(2^numFractionalBits))));
dummyWeights = ones(size(VisionMarker.XProbes))/size(VisionMarker.XProbes,2); % VisionMarkers don't use ProbeWeights like the old Marker2Ds did
maxP = max(max(abs(dummyWeights - double(int16(round(2^numFractionalBits)*dummyWeights))/(2^numFractionalBits))));

maxXPercent = 100 * max(max(abs(VisionMarker.XProbes - double(int16(round(2^numFractionalBits)*VisionMarker.XProbes))/(2^numFractionalBits)) ./ abs(VisionMarker.XProbes)));
maxYPercent = 100 * max(max(abs(VisionMarker.YProbes - double(int16(round(2^numFractionalBits)*VisionMarker.YProbes))/(2^numFractionalBits)) ./ abs(VisionMarker.YProbes)));
maxPPercent = 100 * max(max(abs(dummyWeights - double(int16(round(2^numFractionalBits)*dummyWeights))/(2^numFractionalBits)) ./ abs(dummyWeights)));

fiducialMarkerType = 0;
fiducialMarkerTypeName = sprintf('_FIDICIAL_MARKER_DEFINITION_TYPE_%d_', fiducialMarkerType);

numProbes = size(VisionMarker.XProbes,1);

outputString = sprintf([ ...
    '// Autogenerated by VisionMarker.GenerateEmbeddedMarkerDefinitionCode() on %s\n\n' ...
    '#include "coretech/vision/robot/fiducialMarkers.h"\n\n' ...
    '#ifndef %s\n#define %s\n\n' ...
    '#define NUM_BITS_TYPE_%d %d\n' ...
    '#define NUM_PROBES_PER_BIT_TYPE_%d %d\n' ...
    '#define NUM_FRACTIONAL_BITS_TYPE_%d %d\n\n'], ...
    datestr(now), ...
    fiducialMarkerTypeName, fiducialMarkerTypeName, ...
    fiducialMarkerType, numProbes, ...
    fiducialMarkerType, size(VisionMarker.XProbes, 2), ...
    fiducialMarkerType, numFractionalBits);

% All bits have the same type for VisionMarkers (unlike old Marker2Ds)
outputString = [outputString ...
    sprintf('const Anki::Embedded::FiducialMarkerParserBit::Type bitTypes_type%d[NUM_BITS_TYPE_%d] = {\n', fiducialMarkerType, fiducialMarkerType) ...
    repmat('    Anki::Embedded::FiducialMarkerParserBit::FIDUCIAL_BIT_NONE,\n', 1, numProbes) ...
    '};\n\n'];

outputString = [outputString, sprintf('// SQ%d.%d\nconst short probesX_type%d[NUM_BITS_TYPE_%d][NUM_PROBES_PER_BIT_TYPE_%d] = {\n', ...
    15-numFractionalBits, numFractionalBits, fiducialMarkerType, fiducialMarkerType, fiducialMarkerType)];
for i = 1:numProbes
    outputString = [outputString, '    {', sprintf('%d, ', int32(round(2^numFractionalBits)*VisionMarker.XProbes(i,:))), '},\n']; %#ok<AGROW>
end
outputString = [outputString, '};\n\n'];
    
outputString = [outputString, sprintf('// SQ%d.%d\nconst short probesY_type%d[NUM_BITS_TYPE_%d][NUM_PROBES_PER_BIT_TYPE_%d] = {\n', ...
    15-numFractionalBits, numFractionalBits, fiducialMarkerType, fiducialMarkerType, fiducialMarkerType)];
for i = 1:numProbes
    outputString = [outputString, '    {', sprintf('%d, ', int32(round(2^numFractionalBits)*VisionMarker.YProbes(i,:))), '},\n']; %#ok<AGROW>
end
outputString = [outputString, '};\n\n'];

outputString = [outputString, sprintf('// SQ%d.%d\nconst short probeWeights_type%d[NUM_BITS_TYPE_%d][NUM_PROBES_PER_BIT_TYPE_%d] = {\n', ...
    15-numFractionalBits, numFractionalBits, fiducialMarkerType, fiducialMarkerType, fiducialMarkerType)];
for i = 1:numProbes
    outputString = [outputString, '    {', sprintf('%d, ', int32(round(2^numFractionalBits)*dummyWeights(i,:))), '},\n']; %#ok<AGROW>
end
outputString = [outputString, sprintf('};\n\n#endif //%s\n\n',fiducialMarkerTypeName)];

sprintf(outputString)

sprintf('Maximum differences with %d fractional bits:\n\nTheoretical: %f\n\nMeasured:\nXProbes: %f (%f percent)\nYProbes: %f (%f percent)\nProbeWeights: %f (%f percent)\n',...
    numFractionalBits, 1/(2^(numFractionalBits+1)), maxX, maxXPercent, maxY, maxYPercent, maxP, maxPPercent)

% keyboard
