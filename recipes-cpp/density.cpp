
#include <iostream>
#include <filesystem>
#include <thread>

#include <pdal/PipelineManager.hpp>
#include <pdal/Stage.hpp>
#include <pdal/util/ProgramArgs.hpp>

#include <gdal/gdal_utils.h>

#include "utils.hpp"
#include "alg.hpp"
#include "vpc.hpp"

using namespace pdal;

namespace fs = std::filesystem;

void Density::addArgs()
{
    argOutput = &programArgs.add("output,o", "Output raster file", outputFile);
    argRes = &programArgs.add("resolution,r", "Resolution of the density grid", resolution);
    argTileSize = &programArgs.add("tile-size", "Size of a tile for parallel runs", tileAlignment.tileSize);
    argTileOriginX = &programArgs.add("tile-origin-x", "X origin of a tile for parallel runs", tileAlignment.originX);
    argTileOriginY = &programArgs.add("tile-origin-y", "Y origin of a tile for parallel runs", tileAlignment.originY);
}

bool Density::checkArgs()
{
    if (!argOutput->set())
    {
        std::cerr << "missing output" << std::endl;
        return false;
    }
    if (!argRes->set())
    {
        std::cerr << "missing resolution" << std::endl;
        return false;
    }

    // TODO: not sure why ProgramArgs cleans the default value
    if (!argTileSize->set())
    {
        tileAlignment.tileSize = 1000;
    }

    if (!argTileOriginX->set())
        tileAlignment.originX = -1;
    if (!argTileOriginY->set())
        tileAlignment.originY = -1;

    return true;
}


std::unique_ptr<PipelineManager> Density::pipeline(ParallelJobInfo *tile) const
{
    std::unique_ptr<PipelineManager> manager( new PipelineManager );

    std::vector<Stage*> readers;
    for (const std::string &f : tile->inputFilenames)
    {
        readers.push_back(&manager->makeReader(f, ""));
    }

    if (tile->mode == ParallelJobInfo::Spatial)
    {
        for (Stage* reader : readers)
        {
            // with COPC files, we can also specify bounds at the reader
            // that will only read the required parts of the file
            if (reader->getName() == "readers.copc")
            {
                pdal::Options copc_opts;
                copc_opts.add(pdal::Option("threads", 1));
                copc_opts.add(pdal::Option("bounds", box_to_pdal_bounds(tile->box)));
                reader->addOptions(copc_opts);
            }
        }
    }

    pdal::Options writer_opts;
    writer_opts.add(pdal::Option("binmode", true));
    writer_opts.add(pdal::Option("output_type", "count"));
    writer_opts.add(pdal::Option("resolution", resolution));

    writer_opts.add(pdal::Option("data_type", "int16"));  // 16k points in a cell should be enough? :)
    writer_opts.add(pdal::Option("gdalopts", "TILED=YES"));
    writer_opts.add(pdal::Option("gdalopts", "COMPRESS=DEFLATE"));

    if (tile->box.valid())
    {
        BOX2D box2 = tile->box;
        // fix tile size - PDAL's writers.gdal adds one pixel (see GDALWriter::createGrid()),
        // because it probably expects that that the bounds and resolution do not perfectly match
        box2.maxx -= resolution;
        box2.maxy -= resolution;

        writer_opts.add(pdal::Option("bounds", box_to_pdal_bounds(box2)));
    }

    // TODO: "writers.gdal: Requested driver 'COG' does not support file creation.""
    //   writer_opts.add(pdal::Option("gdaldriver", "COG"));

    pdal::StageCreationOptions opts{ tile->outputFilename, "", nullptr, writer_opts };
    Stage& w = manager->makeWriter( opts );
    for (Stage *stage : readers)
    {
        w.setInput(*stage);  // connect all readers to the writer
    }

    return manager;
}


void Density::preparePipelines(std::vector<std::unique_ptr<PipelineManager>>& pipelines, const BOX3D &bounds, point_count_t &totalPoints)
{
    if (ends_with(inputFile, ".vpc"))
    {
        // using spatial processing

        VirtualPointCloud vpc;
        if (!vpc.read(inputFile))
            return;

        // for /tmp/hello.tif we will use /tmp/hello dir for all results
        fs::path outputParentDir = fs::path(outputFile).parent_path();
        fs::path outputSubdir = outputParentDir / fs::path(outputFile).stem();
        fs::create_directories(outputSubdir);

        bool unalignedFiles = false;

        // TODO: optionally adjust origin to have nicer numbers for bounds?
        if (tileAlignment.originX == -1)
            tileAlignment.originX = bounds.minx;
        if (tileAlignment.originY == -1)
            tileAlignment.originY = bounds.miny;

        // align bounding box of data to the grid
        TileAlignment gridAlignment = tileAlignment;
        gridAlignment.tileSize = resolution;
        Tiling gridTiling = gridAlignment.coverBounds(bounds.to2d());
        std::cout << "grid " << gridTiling.tileCountX << "x" << gridTiling.tileCountY << std::endl;
        BOX2D gridBounds = gridTiling.fullBox();

        Tiling t = tileAlignment.coverBounds(gridBounds);
        std::cout << "tiles " << t.tileCountX << " " << t.tileCountY << std::endl;

        totalPoints = 0;  // we need to recalculate as we may use some points multiple times
        for (int iy = 0; iy < t.tileCountY; ++iy)
        {
            for (int ix = 0; ix < t.tileCountX; ++ix)
            {
                BOX2D tileBox = t.boxAt(ix, iy);

                // for tiles that are smaller than full box - only use intersection
                // to avoid empty areas in resulting rasters
                tileBox.clip(gridBounds);

                ParallelJobInfo tile(ParallelJobInfo::Spatial, tileBox);
                for (const VirtualPointCloud::File & f: vpc.overlappingBox2D(tileBox))
                {
                    tile.inputFilenames.push_back(f.filename);
                    totalPoints += f.count;
                }
                if (tile.inputFilenames.empty())
                    continue;   // no input files for this tile

                if (tile.inputFilenames.size() > 1)
                    unalignedFiles = true;

                // create temp output file names
                // for tile (x=2,y=3) that goes to /tmp/hello.tif,
                // individual output file will be called /tmp/hello/2_3.tif
                fs::path inputBasename = std::to_string(ix) + "_" + std::to_string(iy);
                tile.outputFilename = (outputSubdir / inputBasename).string() + ".tif";

                tileOutputFiles.push_back(tile.outputFilename);

                pipelines.push_back(pipeline(&tile));
            }
        }

        if (unalignedFiles)
        {
            std::cout << std::endl;
            std::cout << "Warning: input files not perfectly aligned with tile grid - processing may take longer." << std::endl;
            std::cout << "Consider using --tile-size, --tile-origin-x, --tile-origin-y arguments" << std::endl;
            std::cout << std::endl;
        }
    }
    else if (ends_with(inputFile, ".copc.laz"))
    {
        // using square tiles for single COPC

        // for /tmp/hello.tif we will use /tmp/hello dir for all results
        fs::path outputParentDir = fs::path(outputFile).parent_path();
        fs::path outputSubdir = outputParentDir / fs::path(outputFile).stem();
        fs::create_directories(outputSubdir);

        if (tileAlignment.originX == -1)
            tileAlignment.originX = bounds.minx;
        if (tileAlignment.originY == -1)
            tileAlignment.originY = bounds.miny;

        Tiling t = tileAlignment.coverBounds(bounds.to2d());

        for (int iy = 0; iy < t.tileCountY; ++iy)
        {
            for (int ix = 0; ix < t.tileCountX; ++ix)
            {
                BOX2D tileBox = t.boxAt(ix, iy);

                ParallelJobInfo tile(ParallelJobInfo::Spatial, tileBox);
                tile.inputFilenames.push_back(inputFile);

                // create temp output file names
                // for tile (x=2,y=3) that goes to /tmp/hello.tif,
                // individual output file will be called /tmp/hello/2_3.tif
                fs::path inputBasename = std::to_string(ix) + "_" + std::to_string(iy);
                tile.outputFilename = (outputSubdir / inputBasename).string() + ".tif";

                tileOutputFiles.push_back(tile.outputFilename);

                pipelines.push_back(pipeline(&tile));
            }
        }
    }
    else
    {
        // single input LAS/LAZ - no parallelism

        ParallelJobInfo tile(ParallelJobInfo::Single);
        tile.inputFilenames.push_back(inputFile);
        tile.outputFilename = outputFile;
        pipelines.push_back(pipeline(&tile));
    }

}


void Density::finalize(std::vector<std::unique_ptr<PipelineManager>>& pipelines)
{
    if (pipelines.size() > 1)
    {
        // build a VRT so that all tiles can be handled as a single data source

        std::string output = outputFile;
        assert(ends_with(output, ".tif"));
        output.erase(output.rfind(".tif"), 4);
        output += ".vrt";

        std::vector<const char*> dsNames;
        for ( const std::string &t : tileOutputFiles )
        {
            dsNames.push_back(t.c_str());
        }

        // https://gdal.org/api/gdal_utils.html
        GDALDatasetH ds = GDALBuildVRT(output.c_str(), (int)dsNames.size(), nullptr, dsNames.data(), nullptr, nullptr);
        assert(ds);

        // export to COG
        // TODO: make this optional?
        const char* args[] = { "-of", "COG", "-co", "COMPRESS=DEFLATE", NULL };
        GDALTranslateOptions* psOptions = GDALTranslateOptionsNew((char**)args, NULL);
        
        GDALDatasetH dsFinal = GDALTranslate(outputFile.c_str(), ds, psOptions, nullptr);
        assert(dsFinal);
        GDALTranslateOptionsFree(psOptions);
        GDALClose(ds);
        GDALClose(dsFinal);

        // TODO: remove VRT + partial tifs after gdal_translate?
     }

}
