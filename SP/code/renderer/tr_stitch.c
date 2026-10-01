/*
===========================================================================

Return to Castle Wolfenstein single player GPL Source Code
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company. 

This file is part of the Return to Castle Wolfenstein single player GPL Source Code (RTCW SP Source Code).  

RTCW SP Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTCW SP Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTCW SP Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the RTCW SP Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the RTCW SP Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

// tr_stitch.c: joins curves (grids) that share an edge so no cracks show
// between them, over a list of surfaces: the map's, at load, or rtcwconv's,
// making a .wld

#include "tr_local.h"

static surfaceType_t **stitchSurfs;
static int numStitchSurfs;

/*
=================
R_MergedWidthPoints

returns true if there are grid points merged on a width edge
=================
*/
int R_MergedWidthPoints( srfGridMesh_t *grid, int offset ) {
	int i, j;

	for ( i = 1; i < grid->width - 1; i++ ) {
		for ( j = i + 1; j < grid->width - 1; j++ ) {
			if ( Q_fabs( grid->verts[i + offset].xyz[0] - grid->verts[j + offset].xyz[0] ) > .1 ) {
				continue;
			}
			if ( Q_fabs( grid->verts[i + offset].xyz[1] - grid->verts[j + offset].xyz[1] ) > .1 ) {
				continue;
			}
			if ( Q_fabs( grid->verts[i + offset].xyz[2] - grid->verts[j + offset].xyz[2] ) > .1 ) {
				continue;
			}
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
R_MergedHeightPoints

returns true if there are grid points merged on a height edge
=================
*/
int R_MergedHeightPoints( srfGridMesh_t *grid, int offset ) {
	int i, j;

	for ( i = 1; i < grid->height - 1; i++ ) {
		for ( j = i + 1; j < grid->height - 1; j++ ) {
			if ( Q_fabs( grid->verts[grid->width * i + offset].xyz[0] - grid->verts[grid->width * j + offset].xyz[0] ) > .1 ) {
				continue;
			}
			if ( Q_fabs( grid->verts[grid->width * i + offset].xyz[1] - grid->verts[grid->width * j + offset].xyz[1] ) > .1 ) {
				continue;
			}
			if ( Q_fabs( grid->verts[grid->width * i + offset].xyz[2] - grid->verts[grid->width * j + offset].xyz[2] ) > .1 ) {
				continue;
			}
			return qtrue;
		}
	}
	return qfalse;
}

/*
=================
R_FixSharedVertexLodError_r

NOTE: never sync LoD through grid edges with merged points!

FIXME: write generalized version that also avoids cracks between a patch and one that meets half way?
=================
*/
void R_FixSharedVertexLodError_r( int start, srfGridMesh_t *grid1 ) {
	int j, k, l, m, n, offset1, offset2, touch;
	srfGridMesh_t *grid2;

	for ( j = start; j < numStitchSurfs; j++ ) {
		//
		grid2 = (srfGridMesh_t *) stitchSurfs[j];
		// if this surface is not a grid
		if ( grid2->surfaceType != SF_GRID ) {
			continue;
		}
		// if the LOD errors are already fixed for this patch
		if ( grid2->lodFixed == 2 ) {
			continue;
		}
		// grids in the same LOD group should have the exact same lod radius
		if ( grid1->lodRadius != grid2->lodRadius ) {
			continue;
		}
		// grids in the same LOD group should have the exact same lod origin
		if ( grid1->lodOrigin[0] != grid2->lodOrigin[0] ) {
			continue;
		}
		if ( grid1->lodOrigin[1] != grid2->lodOrigin[1] ) {
			continue;
		}
		if ( grid1->lodOrigin[2] != grid2->lodOrigin[2] ) {
			continue;
		}
		//
		touch = qfalse;
		for ( n = 0; n < 2; n++ ) {
			//
			if ( n ) {
				offset1 = ( grid1->height - 1 ) * grid1->width;
			} else { offset1 = 0;}
			if ( R_MergedWidthPoints( grid1, offset1 ) ) {
				continue;
			}
			for ( k = 1; k < grid1->width - 1; k++ ) {
				for ( m = 0; m < 2; m++ ) {

					if ( m ) {
						offset2 = ( grid2->height - 1 ) * grid2->width;
					} else { offset2 = 0;}
					if ( R_MergedWidthPoints( grid2, offset2 ) ) {
						continue;
					}
					for ( l = 1; l < grid2->width - 1; l++ ) {
						//
						if ( Q_fabs( grid1->verts[k + offset1].xyz[0] - grid2->verts[l + offset2].xyz[0] ) > .1 ) {
							continue;
						}
						if ( Q_fabs( grid1->verts[k + offset1].xyz[1] - grid2->verts[l + offset2].xyz[1] ) > .1 ) {
							continue;
						}
						if ( Q_fabs( grid1->verts[k + offset1].xyz[2] - grid2->verts[l + offset2].xyz[2] ) > .1 ) {
							continue;
						}
						// ok the points are equal and should have the same lod error
						grid2->widthLodError[l] = grid1->widthLodError[k];
						touch = qtrue;
					}
				}
				for ( m = 0; m < 2; m++ ) {

					if ( m ) {
						offset2 = grid2->width - 1;
					} else { offset2 = 0;}
					if ( R_MergedHeightPoints( grid2, offset2 ) ) {
						continue;
					}
					for ( l = 1; l < grid2->height - 1; l++ ) {
						//
						if ( Q_fabs( grid1->verts[k + offset1].xyz[0] - grid2->verts[grid2->width * l + offset2].xyz[0] ) > .1 ) {
							continue;
						}
						if ( Q_fabs( grid1->verts[k + offset1].xyz[1] - grid2->verts[grid2->width * l + offset2].xyz[1] ) > .1 ) {
							continue;
						}
						if ( Q_fabs( grid1->verts[k + offset1].xyz[2] - grid2->verts[grid2->width * l + offset2].xyz[2] ) > .1 ) {
							continue;
						}
						// ok the points are equal and should have the same lod error
						grid2->heightLodError[l] = grid1->widthLodError[k];
						touch = qtrue;
					}
				}
			}
		}
		for ( n = 0; n < 2; n++ ) {
			//
			if ( n ) {
				offset1 = grid1->width - 1;
			} else { offset1 = 0;}
			if ( R_MergedHeightPoints( grid1, offset1 ) ) {
				continue;
			}
			for ( k = 1; k < grid1->height - 1; k++ ) {
				for ( m = 0; m < 2; m++ ) {

					if ( m ) {
						offset2 = ( grid2->height - 1 ) * grid2->width;
					} else { offset2 = 0;}
					if ( R_MergedWidthPoints( grid2, offset2 ) ) {
						continue;
					}
					for ( l = 1; l < grid2->width - 1; l++ ) {
						//
						if ( Q_fabs( grid1->verts[grid1->width * k + offset1].xyz[0] - grid2->verts[l + offset2].xyz[0] ) > .1 ) {
							continue;
						}
						if ( Q_fabs( grid1->verts[grid1->width * k + offset1].xyz[1] - grid2->verts[l + offset2].xyz[1] ) > .1 ) {
							continue;
						}
						if ( Q_fabs( grid1->verts[grid1->width * k + offset1].xyz[2] - grid2->verts[l + offset2].xyz[2] ) > .1 ) {
							continue;
						}
						// ok the points are equal and should have the same lod error
						grid2->widthLodError[l] = grid1->heightLodError[k];
						touch = qtrue;
					}
				}
				for ( m = 0; m < 2; m++ ) {

					if ( m ) {
						offset2 = grid2->width - 1;
					} else { offset2 = 0;}
					if ( R_MergedHeightPoints( grid2, offset2 ) ) {
						continue;
					}
					for ( l = 1; l < grid2->height - 1; l++ ) {
						//
						if ( Q_fabs( grid1->verts[grid1->width * k + offset1].xyz[0] - grid2->verts[grid2->width * l + offset2].xyz[0] ) > .1 ) {
							continue;
						}
						if ( Q_fabs( grid1->verts[grid1->width * k + offset1].xyz[1] - grid2->verts[grid2->width * l + offset2].xyz[1] ) > .1 ) {
							continue;
						}
						if ( Q_fabs( grid1->verts[grid1->width * k + offset1].xyz[2] - grid2->verts[grid2->width * l + offset2].xyz[2] ) > .1 ) {
							continue;
						}
						// ok the points are equal and should have the same lod error
						grid2->heightLodError[l] = grid1->heightLodError[k];
						touch = qtrue;
					}
				}
			}
		}
		if ( touch ) {
			grid2->lodFixed = 2;
			R_FixSharedVertexLodError_r( start, grid2 );
			//NOTE: this would be correct but makes things really slow
			//grid2->lodFixed = 1;
		}
	}
}

/*
=================
R_FixSharedVertexLodError

This function assumes that all patches in one group are nicely stitched together for the highest LoD.
If this is not the case this function will still do its job but won't fix the highest LoD cracks.
=================
*/
void R_FixSharedVertexLodError( surfaceType_t **surfs, int numSurfs ) {
	int i;
	srfGridMesh_t *grid1;

	stitchSurfs = surfs;
	numStitchSurfs = numSurfs;

	for ( i = 0; i < numStitchSurfs; i++ ) {
		//
		grid1 = (srfGridMesh_t *) stitchSurfs[i];
		// if this surface is not a grid
		if ( grid1->surfaceType != SF_GRID ) {
			continue;
		}
		//
		if ( grid1->lodFixed ) {
			continue;
		}
		//
		grid1->lodFixed = 2;
		// recursively fix other patches in the same LOD group
		R_FixSharedVertexLodError_r( i + 1, grid1 );
	}
}


/*
===============
R_StitchPatches
===============
*/
int R_StitchPatches( int grid1num, int grid2num ) {
	int k, l, m, n, offset1, offset2, row, column;
	srfGridMesh_t *grid1, *grid2;
	float *v1, *v2;

	grid1 = (srfGridMesh_t *) stitchSurfs[grid1num];
	grid2 = (srfGridMesh_t *) stitchSurfs[grid2num];
	for ( n = 0; n < 2; n++ ) {
		//
		if ( n ) {
			offset1 = ( grid1->height - 1 ) * grid1->width;
		} else { offset1 = 0;}
		if ( R_MergedWidthPoints( grid1, offset1 ) ) {
			continue;
		}
		for ( k = 0; k < grid1->width - 2; k += 2 ) {

			for ( m = 0; m < 2; m++ ) {

				if ( grid2->width >= MAX_GRID_SIZE ) {
					break;
				}
				if ( m ) {
					offset2 = ( grid2->height - 1 ) * grid2->width;
				} else { offset2 = 0;}
				//if (R_MergedWidthPoints(grid2, offset2))
				//	continue;
				for ( l = 0; l < grid2->width - 1; l++ ) {
					//
					v1 = grid1->verts[k + offset1].xyz;
					v2 = grid2->verts[l + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}

					v1 = grid1->verts[k + 2 + offset1].xyz;
					v2 = grid2->verts[l + 1 + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}
					//
					v1 = grid2->verts[l + offset2].xyz;
					v2 = grid2->verts[l + 1 + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) < .01 &&
						 Q_fabs( v1[1] - v2[1] ) < .01 &&
						 Q_fabs( v1[2] - v2[2] ) < .01 ) {
						continue;
					}
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert column into grid2 right after after column l
					if ( m ) {
						row = grid2->height - 1;
					} else { row = 0;}
					grid2 = R_GridInsertColumn( grid2, l + 1, row,
												grid1->verts[k + 1 + offset1].xyz, grid1->widthLodError[k + 1] );
					grid2->lodStitched = qfalse;
					stitchSurfs[grid2num] = (void *) grid2;
					return qtrue;
				}
			}
			for ( m = 0; m < 2; m++ ) {

				if ( grid2->height >= MAX_GRID_SIZE ) {
					break;
				}
				if ( m ) {
					offset2 = grid2->width - 1;
				} else { offset2 = 0;}
				//if (R_MergedHeightPoints(grid2, offset2))
				//	continue;
				for ( l = 0; l < grid2->height - 1; l++ ) {
					//
					v1 = grid1->verts[k + offset1].xyz;
					v2 = grid2->verts[grid2->width * l + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}

					v1 = grid1->verts[k + 2 + offset1].xyz;
					v2 = grid2->verts[grid2->width * ( l + 1 ) + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}
					//
					v1 = grid2->verts[grid2->width * l + offset2].xyz;
					v2 = grid2->verts[grid2->width * ( l + 1 ) + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) < .01 &&
						 Q_fabs( v1[1] - v2[1] ) < .01 &&
						 Q_fabs( v1[2] - v2[2] ) < .01 ) {
						continue;
					}
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert row into grid2 right after after row l
					if ( m ) {
						column = grid2->width - 1;
					} else { column = 0;}
					grid2 = R_GridInsertRow( grid2, l + 1, column,
											 grid1->verts[k + 1 + offset1].xyz, grid1->widthLodError[k + 1] );
					grid2->lodStitched = qfalse;
					stitchSurfs[grid2num] = (void *) grid2;
					return qtrue;
				}
			}
		}
	}
	for ( n = 0; n < 2; n++ ) {
		//
		if ( n ) {
			offset1 = grid1->width - 1;
		} else { offset1 = 0;}
		if ( R_MergedHeightPoints( grid1, offset1 ) ) {
			continue;
		}
		for ( k = 0; k < grid1->height - 2; k += 2 ) {
			for ( m = 0; m < 2; m++ ) {

				if ( grid2->width >= MAX_GRID_SIZE ) {
					break;
				}
				if ( m ) {
					offset2 = ( grid2->height - 1 ) * grid2->width;
				} else { offset2 = 0;}
				//if (R_MergedWidthPoints(grid2, offset2))
				//	continue;
				for ( l = 0; l < grid2->width - 1; l++ ) {
					//
					v1 = grid1->verts[grid1->width * k + offset1].xyz;
					v2 = grid2->verts[l + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}

					v1 = grid1->verts[grid1->width * ( k + 2 ) + offset1].xyz;
					v2 = grid2->verts[l + 1 + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}
					//
					v1 = grid2->verts[l + offset2].xyz;
					v2 = grid2->verts[( l + 1 ) + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) < .01 &&
						 Q_fabs( v1[1] - v2[1] ) < .01 &&
						 Q_fabs( v1[2] - v2[2] ) < .01 ) {
						continue;
					}
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert column into grid2 right after after column l
					if ( m ) {
						row = grid2->height - 1;
					} else { row = 0;}
					grid2 = R_GridInsertColumn( grid2, l + 1, row,
												grid1->verts[grid1->width * ( k + 1 ) + offset1].xyz, grid1->heightLodError[k + 1] );
					grid2->lodStitched = qfalse;
					stitchSurfs[grid2num] = (void *) grid2;
					return qtrue;
				}
			}
			for ( m = 0; m < 2; m++ ) {

				if ( grid2->height >= MAX_GRID_SIZE ) {
					break;
				}
				if ( m ) {
					offset2 = grid2->width - 1;
				} else { offset2 = 0;}
				//if (R_MergedHeightPoints(grid2, offset2))
				//	continue;
				for ( l = 0; l < grid2->height - 1; l++ ) {
					//
					v1 = grid1->verts[grid1->width * k + offset1].xyz;
					v2 = grid2->verts[grid2->width * l + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}

					v1 = grid1->verts[grid1->width * ( k + 2 ) + offset1].xyz;
					v2 = grid2->verts[grid2->width * ( l + 1 ) + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}
					//
					v1 = grid2->verts[grid2->width * l + offset2].xyz;
					v2 = grid2->verts[grid2->width * ( l + 1 ) + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) < .01 &&
						 Q_fabs( v1[1] - v2[1] ) < .01 &&
						 Q_fabs( v1[2] - v2[2] ) < .01 ) {
						continue;
					}
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert row into grid2 right after after row l
					if ( m ) {
						column = grid2->width - 1;
					} else { column = 0;}
					grid2 = R_GridInsertRow( grid2, l + 1, column,
											 grid1->verts[grid1->width * ( k + 1 ) + offset1].xyz, grid1->heightLodError[k + 1] );
					grid2->lodStitched = qfalse;
					stitchSurfs[grid2num] = (void *) grid2;
					return qtrue;
				}
			}
		}
	}
	for ( n = 0; n < 2; n++ ) {
		//
		if ( n ) {
			offset1 = ( grid1->height - 1 ) * grid1->width;
		} else { offset1 = 0;}
		if ( R_MergedWidthPoints( grid1, offset1 ) ) {
			continue;
		}
		for ( k = grid1->width - 1; k > 1; k -= 2 ) {

			for ( m = 0; m < 2; m++ ) {

				if ( !grid2 || grid2->width >= MAX_GRID_SIZE ) {
					break;
				}
				if ( m ) {
					offset2 = ( grid2->height - 1 ) * grid2->width;
				} else { offset2 = 0;}
				//if (R_MergedWidthPoints(grid2, offset2))
				//	continue;
				for ( l = 0; l < grid2->width - 1; l++ ) {
					//
					v1 = grid1->verts[k + offset1].xyz;
					v2 = grid2->verts[l + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}

					v1 = grid1->verts[k - 2 + offset1].xyz;
					v2 = grid2->verts[l + 1 + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}
					//
					v1 = grid2->verts[l + offset2].xyz;
					v2 = grid2->verts[( l + 1 ) + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) < .01 &&
						 Q_fabs( v1[1] - v2[1] ) < .01 &&
						 Q_fabs( v1[2] - v2[2] ) < .01 ) {
						continue;
					}
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert column into grid2 right after after column l
					if ( m ) {
						row = grid2->height - 1;
					} else { row = 0;}
					grid2 = R_GridInsertColumn( grid2, l + 1, row,
												grid1->verts[k - 1 + offset1].xyz, grid1->widthLodError[k + 1] );
					grid2->lodStitched = qfalse;
					stitchSurfs[grid2num] = (void *) grid2;
					return qtrue;
				}
			}
			for ( m = 0; m < 2; m++ ) {

				if ( !grid2 || grid2->height >= MAX_GRID_SIZE ) {
					break;
				}
				if ( m ) {
					offset2 = grid2->width - 1;
				} else { offset2 = 0;}
				//if (R_MergedHeightPoints(grid2, offset2))
				//	continue;
				for ( l = 0; l < grid2->height - 1; l++ ) {
					//
					v1 = grid1->verts[k + offset1].xyz;
					v2 = grid2->verts[grid2->width * l + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}

					v1 = grid1->verts[k - 2 + offset1].xyz;
					v2 = grid2->verts[grid2->width * ( l + 1 ) + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}
					//
					v1 = grid2->verts[grid2->width * l + offset2].xyz;
					v2 = grid2->verts[grid2->width * ( l + 1 ) + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) < .01 &&
						 Q_fabs( v1[1] - v2[1] ) < .01 &&
						 Q_fabs( v1[2] - v2[2] ) < .01 ) {
						continue;
					}
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert row into grid2 right after after row l
					if ( m ) {
						column = grid2->width - 1;
					} else { column = 0;}
					grid2 = R_GridInsertRow( grid2, l + 1, column,
											 grid1->verts[k - 1 + offset1].xyz, grid1->widthLodError[k + 1] );
					if ( !grid2 ) {
						break;
					}
					grid2->lodStitched = qfalse;
					stitchSurfs[grid2num] = (void *) grid2;
					return qtrue;
				}
			}
		}
	}
	for ( n = 0; n < 2; n++ ) {
		//
		if ( n ) {
			offset1 = grid1->width - 1;
		} else { offset1 = 0;}
		if ( R_MergedHeightPoints( grid1, offset1 ) ) {
			continue;
		}
		for ( k = grid1->height - 1; k > 1; k -= 2 ) {
			for ( m = 0; m < 2; m++ ) {

				if ( !grid2 || grid2->width >= MAX_GRID_SIZE ) {
					break;
				}
				if ( m ) {
					offset2 = ( grid2->height - 1 ) * grid2->width;
				} else { offset2 = 0;}
				//if (R_MergedWidthPoints(grid2, offset2))
				//	continue;
				for ( l = 0; l < grid2->width - 1; l++ ) {
					//
					v1 = grid1->verts[grid1->width * k + offset1].xyz;
					v2 = grid2->verts[l + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}

					v1 = grid1->verts[grid1->width * ( k - 2 ) + offset1].xyz;
					v2 = grid2->verts[l + 1 + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}
					//
					v1 = grid2->verts[l + offset2].xyz;
					v2 = grid2->verts[( l + 1 ) + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) < .01 &&
						 Q_fabs( v1[1] - v2[1] ) < .01 &&
						 Q_fabs( v1[2] - v2[2] ) < .01 ) {
						continue;
					}
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert column into grid2 right after after column l
					if ( m ) {
						row = grid2->height - 1;
					} else { row = 0;}
					grid2 = R_GridInsertColumn( grid2, l + 1, row,
												grid1->verts[grid1->width * ( k - 1 ) + offset1].xyz, grid1->heightLodError[k + 1] );
					grid2->lodStitched = qfalse;
					stitchSurfs[grid2num] = (void *) grid2;
					return qtrue;
				}
			}
			for ( m = 0; m < 2; m++ ) {

				if ( !grid2 || grid2->height >= MAX_GRID_SIZE ) {
					break;
				}
				if ( m ) {
					offset2 = grid2->width - 1;
				} else { offset2 = 0;}
				//if (R_MergedHeightPoints(grid2, offset2))
				//	continue;
				for ( l = 0; l < grid2->height - 1; l++ ) {
					//
					v1 = grid1->verts[grid1->width * k + offset1].xyz;
					v2 = grid2->verts[grid2->width * l + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}

					v1 = grid1->verts[grid1->width * ( k - 2 ) + offset1].xyz;
					v2 = grid2->verts[grid2->width * ( l + 1 ) + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[1] - v2[1] ) > .1 ) {
						continue;
					}
					if ( Q_fabs( v1[2] - v2[2] ) > .1 ) {
						continue;
					}
					//
					v1 = grid2->verts[grid2->width * l + offset2].xyz;
					v2 = grid2->verts[grid2->width * ( l + 1 ) + offset2].xyz;
					if ( Q_fabs( v1[0] - v2[0] ) < .01 &&
						 Q_fabs( v1[1] - v2[1] ) < .01 &&
						 Q_fabs( v1[2] - v2[2] ) < .01 ) {
						continue;
					}
					//
					//ri.Printf( PRINT_ALL, "found highest LoD crack between two patches\n" );
					// insert row into grid2 right after after row l
					if ( m ) {
						column = grid2->width - 1;
					} else { column = 0;}
					grid2 = R_GridInsertRow( grid2, l + 1, column,
											 grid1->verts[grid1->width * ( k - 1 ) + offset1].xyz, grid1->heightLodError[k + 1] );
					grid2->lodStitched = qfalse;
					stitchSurfs[grid2num] = (void *) grid2;
					return qtrue;
				}
			}
		}
	}
	return qfalse;
}

/*
===============
R_TryStitchPatch

This function will try to stitch patches in the same LoD group together for the highest LoD.

Only single missing vertice cracks will be fixed.

Vertices will be joined at the patch side a crack is first found, at the other side
of the patch (on the same row or column) the vertices will not be joined and cracks
might still appear at that side.
===============
*/
int R_TryStitchingPatch( int grid1num ) {
	int j, numstitches;
	srfGridMesh_t *grid1, *grid2;

	numstitches = 0;
	grid1 = (srfGridMesh_t *) stitchSurfs[grid1num];
	for ( j = 0; j < numStitchSurfs; j++ ) {
		//
		grid2 = (srfGridMesh_t *) stitchSurfs[j];
		// if this surface is not a grid
		if ( grid2->surfaceType != SF_GRID ) {
			continue;
		}
		// grids in the same LOD group should have the exact same lod radius
		if ( grid1->lodRadius != grid2->lodRadius ) {
			continue;
		}
		// grids in the same LOD group should have the exact same lod origin
		if ( grid1->lodOrigin[0] != grid2->lodOrigin[0] ) {
			continue;
		}
		if ( grid1->lodOrigin[1] != grid2->lodOrigin[1] ) {
			continue;
		}
		if ( grid1->lodOrigin[2] != grid2->lodOrigin[2] ) {
			continue;
		}
		//
		while ( R_StitchPatches( grid1num, j ) )
		{
			numstitches++;
		}
	}
	return numstitches;
}

/*
===============
R_StitchAllPatches
===============
*/
void R_StitchAllPatches( surfaceType_t **surfs, int numSurfs ) {
	int i, stitched, numstitches;
	srfGridMesh_t *grid1;

	stitchSurfs = surfs;
	numStitchSurfs = numSurfs;

	numstitches = 0;
	do
	{
		stitched = qfalse;
		for ( i = 0; i < numStitchSurfs; i++ ) {
			//
			grid1 = (srfGridMesh_t *) stitchSurfs[i];
			// if this surface is not a grid
			if ( grid1->surfaceType != SF_GRID ) {
				continue;
			}
			//
			if ( grid1->lodStitched ) {
				continue;
			}
			//
			grid1->lodStitched = qtrue;
			stitched = qtrue;
			//
			numstitches += R_TryStitchingPatch( i );
		}
	}
	while ( stitched );
	ri.Printf( PRINT_ALL, "stitched %d LoD cracks\n", numstitches );
}
