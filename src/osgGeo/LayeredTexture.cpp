/* osgGeo - A collection of geoscientific extensions to OpenSceneGraph.
Copyright 2011 dGB Beheer B.V.

osgGeo is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>

$Id$

*/


#include <osgGeo/LayeredTexture>
#include <osg/BlendFunc>
#include <osg/FragmentProgram>
#include <osg/Geometry>
#include <osg/State>
#include <osg/Texture2D>
#include <osg/Version>
#include <osg/VertexProgram>
#include <osgUtil/CullVisitor>
#include <osgGeo/Vec2i>

#include <string.h>
#include <iostream>
#include <cstdio>


#if OSG_MIN_VERSION_REQUIRED(3,1,0)
     #define USE_IMAGE_STRIDE
#endif

namespace osgGeo
{


int LayeredTexture::powerOf2Ceil( unsigned short nr )
{
    if ( nr<=256 )
    {
	if ( nr<=16 )
	{
	    if ( nr<=4 )
		return nr<=2 ? nr : 4;

	    return nr<=8 ? 8 : 16;
	}

	if ( nr<=64 )
	    return nr<=32 ? 32 : 64;

	return nr<=128 ? 128 : 256;
    }

    if ( nr<=4096 )
    {
	if ( nr<=1024 )
	    return nr<=512 ? 512 : 1024;

	return nr<=2048 ? 2048 : 4096;
    }

    if ( nr<=16384 )
	return nr<=8192 ? 8192 : 16384;

    return nr<=32768 ? 32768 : 65536;
}


int LayeredTexture::image2TextureChannel( int channel, GLenum format )
{
    if ( channel<0 || channel>3 )
	return -1;

    if ( format==GL_RGBA )
	return channel;
    if ( format==GL_RGB )
	return channel==3 ? -1 : channel;
    if ( format==GL_LUMINANCE_ALPHA )
	return channel>1 ? -1 : 3*channel;
    if ( format==GL_LUMINANCE || format==GL_INTENSITY ||
	 format==GL_RED || format==GL_DEPTH_COMPONENT )
	return channel==0 ? 0 : -1;
    if ( format==GL_ALPHA )
	return channel==0 ? 3 : -1;
    if ( format==GL_GREEN )
	return channel==0 ? 1 : -1;
    if ( format==GL_BLUE )
	return channel==0 ? 2 : -1;
    if ( format==GL_BGRA )
	return channel==3 ? 3 : 2-channel;
    if ( format==GL_BGR )
	return channel==3 ? -1 : 2-channel;

    return -1;
}


// Beware that osg::Image::getColor(.,.,.) is used occasionally, but
// not (yet) supports format: GL_RED, GL_GREEN, GL_BLUE, GL_INTENSITY

#define ONE_CHANNEL  4
#define ZERO_CHANNEL 5

static int texture2ImageChannel( int channel, GLenum format )
{
    if ( channel<0 || channel>3 )
	return -1;

    if ( format==GL_RGBA )
	return channel;
    if ( format==GL_RGB )
	return channel==3 ? ONE_CHANNEL : channel;
    if ( format==GL_LUMINANCE_ALPHA )
	return channel==3 ? 1 : 0;
    if ( format==GL_LUMINANCE || format==GL_DEPTH_COMPONENT )
	return channel==3 ? ONE_CHANNEL : 0;
    if ( format==GL_ALPHA )
	return channel==3 ? 0 : ZERO_CHANNEL;
    if ( format==GL_INTENSITY )
	return 0;
    if ( format==GL_RED )
	return channel==0 ? 0 : (channel==3 ? ONE_CHANNEL : ZERO_CHANNEL);
    if ( format==GL_GREEN )
	return channel==1 ? 0 : (channel==3 ? ONE_CHANNEL : ZERO_CHANNEL); 
    if ( format==GL_BLUE )
	return channel==2 ? 0 : (channel==3 ? ONE_CHANNEL : ZERO_CHANNEL);
    if ( format==GL_BGRA )
	return channel==3 ? 3 : 2-channel;
    if ( format==GL_BGR )
	return channel==3 ? ONE_CHANNEL : 2-channel;

    return -1;
}


static TransparencyType getImageTransparencyType( const osg::Image* image, int textureChannel=3 )
{                                                                               
    if ( !image )
	return FullyTransparent;

    GLenum format = image->getPixelFormat();
    const int imageChannel = texture2ImageChannel( textureChannel, format );

    if ( imageChannel==ZERO_CHANNEL )
	return FullyTransparent;
    if ( imageChannel==ONE_CHANNEL )
	return Opaque;

    GLenum dataType = image->getDataType();                               
    bool isByte = dataType==GL_UNSIGNED_BYTE || dataType==GL_BYTE;

    if ( isByte && imageChannel>=0 )
    {
	const int step = image->getPixelSizeInBits()/8;
	const unsigned char* start = image->data()+imageChannel;
	const unsigned char* stop = start+image->getTotalSizeInBytes()-step;
	return getTransparencyTypeBytewise( start, stop, step ); 
    }

    bool foundOpaquePixel = false;
    bool foundTransparentPixel = false;

    for ( int r=image->r()-1; r>=0; r-- )
    {
	for ( int t=image->t()-1; t>=0; t-- )
	{
	    for ( int s=image->s()-1; s>=0; s-- )
	    {
		const float val = image->getColor(s,t,r)[imageChannel];
		if ( val<=0.0f )
		    foundTransparentPixel = true;
		else if ( val>=1.0f )
		    foundOpaquePixel = true;
		else
		    return HasTransparencies;
	    }
	}
    }

    if ( foundTransparentPixel )
	return foundOpaquePixel ? OnlyFullTransparencies : FullyTransparent;

    return Opaque;
}


//============================================================================


struct LayeredTextureData : public osg::Referenced
{
			LayeredTextureData(int id)
			    : _id( id )
			    , _origin( 0.0f, 0.0f )
			    , _scale( 1.0f, 1.0f )
			    , _image( 0 )
			    , _imageSource( 0 )
			    , _imageScale( 1.0f, 1.0f )
			    , _freezeDisplay( false )
			    , _textureUnit( -1 )
			    , _filterType(Linear)
			    , _borderColor( 1.0f, 1.0f, 1.0f, 1.0f )
			    , _borderColorSource( 1.0f, 1.0f, 1.0f, 1.0f )
			    , _undefLayerId( -1 )
			    , _undefChannel( 0 )
			    , _undefColor( -1.0f, -1.0f, -1.0f, -1.0f )
			    , _undefColorSource( -1.0f, -1.0f, -1.0f, -1.0f )
			    , _dirtyTileImages( false )
			{
			    for ( int idx=0; idx<4; idx++ )
				_undefChannelRefCount[idx] = 0;
			}

			~LayeredTextureData();

    LayeredTextureData*	clone() const;
    osg::Vec2f		getLayerCoord(const osg::Vec2f& global) const;
    osg::Vec4f		getTextureVec(const osg::Vec2f& global) const;
    void		clearTransparencyType();
    void		adaptColors();
    void		cleanUp();
    void		updateTileImagesIfNeeded() const;

    const int					_id;
    osg::Vec2f					_origin;
    osg::Vec2f					_scale;
    osg::ref_ptr<const osg::Image>		_image;
    osg::ref_ptr<const osg::Image>		_imageSource;
    Vec2i					_imageSourceSize;
    const unsigned char*			_imageSourceData;
    osg::Vec2f					_imageScale;
    int						_imageModifiedCount;
    bool					_imageModifiedFlag;
    bool					_freezeDisplay;
    int						_textureUnit;
    FilterType					_filterType;

    osg::Vec4f					_borderColor;
    osg::Vec4f					_borderColorSource;
    int						_undefLayerId;
    int						_undefChannel;
    osg::Vec4f					_undefColor;
    osg::Vec4f					_undefColorSource;
    int						_undefChannelRefCount[4];
    TransparencyType				_transparency[4];

    mutable std::vector<osg::Image*>		_tileImages;
    mutable bool				_dirtyTileImages;
};


LayeredTextureData::~LayeredTextureData()
{
    cleanUp();
}


LayeredTextureData* LayeredTextureData::clone() const
{
    LayeredTextureData* res = new LayeredTextureData( _id );
    res->_origin = _origin;
    res->_scale = _scale; 
    res->_textureUnit = _textureUnit;
    res->_filterType = _filterType;
    res->_freezeDisplay = _freezeDisplay;
    res->_imageModifiedCount = _imageModifiedCount;
    res->_imageScale = _imageScale; 
    res->_imageSource = _imageSource.get();

    if ( _image.get()==_imageSource.get() )
	res->_image = _image.get();
    else
	res->_image = (osg::Image*) _image->clone(osg::CopyOp::DEEP_COPY_ALL);

    res->_borderColor = _borderColor;
    res->_borderColor = _borderColorSource;
    res->_undefLayerId = _undefLayerId;
    res->_undefChannel = _undefChannel;
    res->_undefColorSource = _undefColorSource;
    res->_undefColor = _undefColor;
    res->_dirtyTileImages = _dirtyTileImages;

    for ( int idx=0; idx<4; idx++ )
    {
	res->_undefChannelRefCount[idx] = _undefChannelRefCount[idx];
	res->_transparency[idx] = _transparency[idx];
    }

    return res;
}


osg::Vec2f LayeredTextureData::getLayerCoord( const osg::Vec2f& global ) const
{
    osg::Vec2f res = global - _origin;
    res.x() /= _scale.x() * _imageScale.x();
    res.y() /= _scale.y() * _imageScale.y();
    
    return res;
}


void LayeredTextureData::clearTransparencyType()
{
    for ( int idx=0; idx<4; idx++ )
	_transparency[idx] = TransparencyUnknown;
}


void LayeredTextureData::adaptColors()
{
    _undefColor  = _undefColorSource;
    _borderColor = _borderColorSource;

    if ( !_image )
	return;

    const GLenum format = _image->getPixelFormat();

    for ( int idx=0; idx<4; idx++ )
    {
	const int ic = texture2ImageChannel( idx, format );

	if ( ic==ZERO_CHANNEL )
	{
	    _undefColor[idx]  = _undefColor[idx]<0.0f  ? -1.0f : 0.0f;
	    _borderColor[idx] = _borderColor[idx]<0.0f ? -1.0f : 0.0f;
	}
	else if ( ic==ONE_CHANNEL )
	{
	    _undefColor[idx]  = _undefColor[idx]<0.0f  ? -1.0f : 1.0f;
	    _borderColor[idx] = _borderColor[idx]<0.0f ? -1.0f : 1.0f;
	}
	else if ( ic>=0 )
	{
	    const int tc = osgGeo::LayeredTexture::image2TextureChannel( ic, format );
	    _undefColor[idx]  = _undefColorSource[tc];
	    _borderColor[idx] = _borderColorSource[tc];
	}
    }
}


#define GET_COLOR( color, image, s, t ) \
\
    osg::Vec4f color = _borderColor; \
    if ( s>=0 && s<image->s() && t>=0 && t<image->t() ) \
	color = image->getColor( s, t ); \
    else if ( _borderColor[0]>=0.0f ) \
	color = _borderColor; \
    else \
    { \
	const int sClamp = s<=0 ? 0 : ( s>=image->s() ? image->s()-1 : s ); \
	const int tClamp = t<=0 ? 0 : ( t>=image->t() ? image->t()-1 : t ); \
	color = image->getColor( sClamp, tClamp ); \
    }

osg::Vec4f LayeredTextureData::getTextureVec( const osg::Vec2f& globalCoord ) const
{
    if ( !_image.get() || !_image->s() || !_image->t() )
	return _borderColor;

    osg::Vec2f local = getLayerCoord( globalCoord );
    if ( _filterType!=Nearest )
	local -= osg::Vec2f( 0.5, 0.5 );

    int s = (int) floor( local.x() );
    int t = (int) floor( local.y() );

    GET_COLOR( col00, _image, s, t );

    if ( _filterType==Nearest )
	return col00;

    const float sFrac = local.x()-s;
    const float tFrac = local.y()-t;

    if ( !tFrac )
    {
	if ( !sFrac )
	    return col00;

	s++;
	GET_COLOR( col10, _image, s, t );
	return col00*(1.0f-sFrac) + col10*sFrac;
    }

    t++;
    GET_COLOR( col01, _image, s, t );
    col00 = col00*(1.0f-tFrac) + col01*tFrac;

    if ( !sFrac )
	return col00;

    s++;
    GET_COLOR( col11, _image, s, t );
    t--;
    GET_COLOR( col10, _image, s, t );

    col10 = col10*(1.0f-tFrac) + col11*tFrac;
    return  col00*(1.0f-sFrac) + col10*sFrac;
}


void LayeredTextureData::cleanUp()
{
    std::vector<osg::Image*>::iterator it = _tileImages.begin();
    for ( ; it!=_tileImages.end(); it++ )
	(*it)->unref();

    _tileImages.clear();
}


void LayeredTextureData::updateTileImagesIfNeeded() const
{
    if ( _dirtyTileImages )
    {
	std::vector<osg::Image*>::iterator it = _tileImages.begin();
	for ( ; it!=_tileImages.end(); it++ )
	    (*it)->dirty();

	_dirtyTileImages = false;
    }
}


//============================================================================


struct TilingInfo
{
			TilingInfo()			{ reInit(); }

    void		reInit()
			{
			    _envelopeOrigin = osg::Vec2f( 0.0f, 0.0f );
			    _envelopeSize = osg::Vec2f( 0.0f, 0.0f );
			    _smallestScale = osg::Vec2f( 1.0f, 1.0f );
			    _maxTileSize = osg::Vec2f( 0.0f, 0.0f );
			    _needsUpdate = false;
			    _retilingNeeded = true;
			}

    osg::Vec2f		_envelopeOrigin;
    osg::Vec2f		_envelopeSize;
    osg::Vec2f		_smallestScale;
    osg::Vec2f  	_maxTileSize;
    bool		_needsUpdate;
    bool		_retilingNeeded;
};


struct TextureInfo
{
    				TextureInfo()
				    : _isValid( false )
				    , _contextId( -1 )
				    , _nrUnits( 256 )
				    , _maxSize( 65536 )
				    , _nonPowerOf2Support( false )
				    , _shadingSupport( false )
				{}

    bool			_isValid;
    int				_contextId;
    int				_nrUnits;
    int				_maxSize;
    bool			_nonPowerOf2Support;
    bool			_shadingSupport;
};


//============================================================================

#define EPS			1e-5
#define START_RECYCLING_ID	100

LayeredTexture::LayeredTexture()
    : _updateSetupStateSet( false )
    , _maxTextureCopySize( 32*32 )
    , _textureSizePolicy( PowerOf2 )
    , _seamPower( 0, 0 )
    , _externalTexelSizeRatio( 1.0 )
    , _tilingInfo( new TilingInfo )
    , _texInfo( new TextureInfo )
    , _stackUndefLayerId( -1 )
    , _stackUndefChannel( 0 )
    , _stackUndefColor( 0.0f, 0.0f, 0.0f, 0.0f )
    , _invertUndefLayers( false )
    , _allowShaders( true )
    , _maySkipEarlyProcesses( false )
    , _useShaders( false )
    , _compositeLayerUpdate( true )
    , _retileCompositeLayer( false )
    , _reInitTiling( false )
    , _isOn( true )
{
    _id2idxTable.push_back( -1 );	// ID=0 used to represent ColSeqTexture

    _compositeLayerId = addDataLayer();
}


LayeredTexture::LayeredTexture( const LayeredTexture& lt,
				const osg::CopyOp& co )
    : osgGeo::CallbackObject( lt, co )
    , _updateSetupStateSet( false )
    , _setupStateSet( 0 )
    , _maxTextureCopySize( lt._maxTextureCopySize )
    , _textureSizePolicy( lt._textureSizePolicy )
    , _seamPower( lt._seamPower )
    , _externalTexelSizeRatio( lt._externalTexelSizeRatio )
    , _tilingInfo( new TilingInfo(*lt._tilingInfo) )
    , _texInfo( new TextureInfo(*lt._texInfo) )
    , _stackUndefLayerId( lt._stackUndefLayerId )
    , _stackUndefChannel( lt._stackUndefChannel )
    , _stackUndefColor( lt._stackUndefColor )
    , _allowShaders( lt._allowShaders )
    , _maySkipEarlyProcesses( lt._maySkipEarlyProcesses )
    , _useShaders( lt._useShaders )
    , _compositeLayerId( lt._compositeLayerId )
    , _compositeLayerUpdate( lt._compositeLayerUpdate )
    , _retileCompositeLayer( false )
    , _reInitTiling( false )
    , _isOn( lt._isOn )
{
    for ( unsigned int idx=0; idx<lt._dataLayers.size(); idx++ )
    {
	osg::ref_ptr<LayeredTextureData> layer =
		co.getCopyFlags()==osg::CopyOp::DEEP_COPY_ALL
	    ? lt._dataLayers[idx]->clone()
	    : lt._dataLayers[idx];

	layer->ref();
	_dataLayers.push_back( layer );
    }

    for ( unsigned int idx=0; idx<lt._id2idxTable.size(); idx++ )
	_id2idxTable.push_back( lt._id2idxTable[idx] );
    for ( unsigned int idx=0; idx<lt._releasedIds.size(); idx++ )
	_releasedIds.push_back( lt._releasedIds[idx] );
}


LayeredTexture::~LayeredTexture()
{
    std::for_each( _dataLayers.begin(), _dataLayers.end(),
	    	   osg::intrusive_ptr_release );

    std::for_each( _processes.begin(), _processes.end(),
	    	   osg::intrusive_ptr_release );

    delete _tilingInfo;
    delete _texInfo;
}


void LayeredTexture::setUpdateVar( bool& variable, bool yn )
{
/*  Very suitable spot for breakpoints when debugging update issues
    if ( &variable == &_tilingInfo->_needsUpdate )
	std::cout << "_tilingInfo->_needsUpdate = " << yn << std::endl;
    if ( &variable == &_tilingInfo->_retilingNeeded )
	std::cout << "_tilingInfo->_retilingNeeded = " << yn << std::endl;
    if ( &variable == & _updateSetupStateSet )
	std::cout << " _updateSetupStateSet = " << yn << std::endl;
*/

    if ( !variable && yn )
	triggerRedrawRequest();

    variable = yn;
}


void LayeredTexture::turnOn( bool yn )
{
    if ( _isOn!=yn )
    {
	setUpdateVar( _tilingInfo->_retilingNeeded, true );
	_isOn = yn;
    }
}


int LayeredTexture::addDataLayer()
{
    _lock.writeLock();

    unsigned int freeId = _id2idxTable.size();
    if ( freeId>=START_RECYCLING_ID && !_releasedIds.empty() )
    {
	freeId = *_releasedIds.begin();
	_releasedIds.erase( _releasedIds.begin() );
    }

    osg::ref_ptr<LayeredTextureData> ltd = new LayeredTextureData( freeId );
    if ( ltd )
    {
	if ( freeId<_id2idxTable.size() )
	    _id2idxTable[freeId] = _dataLayers.size();
	else
	    _id2idxTable.push_back( _dataLayers.size() );

	ltd->ref();
	_dataLayers.push_back( ltd );
    }

    _lock.writeUnlock();
    return ltd ? ltd->_id : -1;
}


void LayeredTexture::raiseUndefChannelRefCount( bool yn, int idx )
{
    int udfIdx = getDataLayerIndex( _stackUndefLayerId );
    int channel = _stackUndefChannel;

    if ( idx>=0 && idx<nrDataLayers() )
    {
	udfIdx = getDataLayerIndex( _dataLayers[idx]->_undefLayerId );
	channel = _dataLayers[idx]->_undefChannel;
    }

    if ( udfIdx!=-1 )
    {
	_dataLayers[udfIdx]->_undefChannelRefCount[channel] += (yn ? 1 : -1);

	if ( _dataLayers[udfIdx]->_textureUnit>=0 )
	{
	    const int cnt = _dataLayers[udfIdx]->_undefChannelRefCount[channel];
	    if ( !cnt || (yn && cnt==1) )
		 setUpdateVar( _tilingInfo->_retilingNeeded, true );
	 }
    }
}


void LayeredTexture::removeDataLayer( int id )
{
    if ( id==_compositeLayerId )
	return; 

    _lock.writeLock();
    int idx = getDataLayerIndex( id );
    if ( idx!=-1 )
    {
	raiseUndefChannelRefCount( false, idx );

	for ( int tc=0; tc<4; tc++ )
	{
	    if ( _dataLayers[idx]->_undefChannelRefCount[tc]>0 )
	    {
		std::cerr << "Broken link to undef layer" << std::endl;
		break;
	    }

	    if ( tc==3 )
		_releasedIds.push_back( id );
	}

	osg::ref_ptr<LayeredTextureData> ltd = _dataLayers[idx];
	_dataLayers.erase( _dataLayers.begin()+idx );
	setUpdateVar( _tilingInfo->_needsUpdate, true );
	ltd->unref();

	_id2idxTable[id] = -1;
	while ( idx < (int)_dataLayers.size() )
	    _id2idxTable[_dataLayers[idx++]->_id]--;
    }

    _lock.writeUnlock();
}


int LayeredTexture::getDataLayerID( int idx ) const
{
    return idx>=0 && idx<(int)_dataLayers.size() ? _dataLayers[idx]->_id : -1;
}


int LayeredTexture::getDataLayerIndex( int id ) const
{
    return id>=0 && id<(int)_id2idxTable.size() ? _id2idxTable[id] : -1;
}


bool LayeredTexture::isDataLayerOK( int id ) const
{
    return getDataLayerImage( id );
}


void LayeredTexture::setDataLayerOrigin( int id, const osg::Vec2f& origin )
{
    const int idx = getDataLayerIndex( id );
    if ( idx!=-1 )
    {
	_dataLayers[idx]->_origin = origin; 
	setUpdateVar( _tilingInfo->_needsUpdate, true );
    }
}


void LayeredTexture::setDataLayerScale( int id, const osg::Vec2f& scale )
{
    const int idx = getDataLayerIndex( id );
    if ( idx!=-1 && scale.x()>=0.0f && scale.y()>0.0f )
    {
	_dataLayers[idx]->_scale = scale;
	setUpdateVar( _tilingInfo->_needsUpdate, true );
    }
}


void LayeredTexture::setDataLayerImage( int id, const osg::Image* image, bool freezewhile0 )
{
    const int idx = getDataLayerIndex( id );
    if ( idx==-1 )
	return;

    LayeredTextureData& layer = *_dataLayers[idx];

    if ( freezewhile0 && !image && layer._imageSource.get() )
	setUpdateVar( layer._freezeDisplay, true );

    if ( image )
    {
	setUpdateVar( layer._freezeDisplay, false );

	if ( !image->s() || !image->t() || !image->getPixelFormat() )
	{
	    std::cerr << "Data layer image cannot be set before allocation" << std::endl;
	    return;
	}

	Vec2i newImageSize( image->s(), image->t() );

#ifdef USE_IMAGE_STRIDE
	const bool retile = layer._imageSource.get()!=image || layer._imageSourceData!=image->data() || layer._imageSourceSize!=newImageSize || !layer._tileImages.size();
#else
	const bool retile = true;
#endif

	const int s = powerOf2Ceil( image->s() );
	const int t = powerOf2Ceil( image->t() );

	bool scaleImage = s>image->s() || t>image->t();
	if ( image->s()>=8 && image->t()>=8 && s*t>int(_maxTextureCopySize) )
	    scaleImage = false;

	if ( scaleImage && _textureSizePolicy!=AnySize && id!=_compositeLayerId )
	{
	    osg::Image* imageCopy = new osg::Image( *image );
	    imageCopy->scaleImage( s, t, image->r() );

	    if ( !retile )
		const_cast<osg::Image*>(layer._image.get())->copySubImage( 0, 0, 0, imageCopy ); 
	    else
		layer._image = imageCopy;

	    layer._imageScale.x() = float(image->s()) / float(s);
	    layer._imageScale.y() = float(image->t()) / float(t);
	}
	else
	{
	    layer._image = image;
	    layer._imageScale = osg::Vec2f( 1.0f, 1.0f );
	}

	layer._imageSource = image;
	layer._imageSourceData = image->data();
	layer._imageSourceSize = newImageSize;
	layer._imageModifiedCount = image->getModifiedCount();
	layer.clearTransparencyType();

	if ( retile )
	{
	    layer.adaptColors();
	    setUpdateVar( _tilingInfo->_needsUpdate, true );
	}
	else
	    setUpdateVar( layer._dirtyTileImages, true );
    }
    else if ( layer._image )
    {
	layer._image = 0; 
	layer._imageSource = 0;
	layer.adaptColors();
	setUpdateVar( _tilingInfo->_needsUpdate, true );
    }
}


void LayeredTexture::setDataLayerUndefLayerID( int id, int undefId )
{
    const int idx = getDataLayerIndex( id );
    if ( idx!=-1 )
    {
	raiseUndefChannelRefCount( false, idx );

	setUpdateVar( _updateSetupStateSet, true );
	_dataLayers[idx]->_undefLayerId = undefId;

	raiseUndefChannelRefCount( true, idx );
    }
}


void LayeredTexture::setDataLayerUndefChannel( int id, int channel )
{
    const int idx = getDataLayerIndex( id );
    if ( idx!=-1 && channel>=0 && channel<4 )
    {
	raiseUndefChannelRefCount( false, idx );

	setUpdateVar( _updateSetupStateSet, true );

	_dataLayers[idx]->_undefChannel = channel;
	if ( _dataLayers[idx]->_undefLayerId<0 )
	    _dataLayers[idx]->_undefLayerId = id;

	raiseUndefChannelRefCount( true, idx );
    }
}
   

void LayeredTexture::setDataLayerImageUndefColor( int id, const osg::Vec4f& col )
{
    const int idx = getDataLayerIndex( id );
    if ( idx!=-1 )
    {
	for ( int tc=0; tc<4; tc++ )
	{
	    _dataLayers[idx]->_undefColorSource[tc] =
		col[tc]<0.0f ? -1.0f : ( col[tc]>=1.0f ? 1.0f : col[tc] );
	}

	_dataLayers[idx]->adaptColors();
	if ( _dataLayers[idx]->_textureUnit>=0 )
	    setUpdateVar( _updateSetupStateSet, true );
    }
}


void LayeredTexture::setDataLayerBorderColor( int id, const osg::Vec4f& col )
{
    const int idx = getDataLayerIndex( id );
    if ( idx!=-1 )
    {
	_dataLayers[idx]->_borderColorSource = osg::Vec4f(-1.0f,-1.0f,-1.0f,-1.0f);
	if ( col[0]>=0.0f && col[1]>=0.0f && col[2]>=0.0f && col[3]>=0.0f )
	{
	    for ( int tc=0; tc<4; tc++ )
	    {
		_dataLayers[idx]->_borderColorSource[tc] = col[tc]>=1.0f ? 1.0f : col[tc];
	    }
	}

	_dataLayers[idx]->adaptColors();
	if ( _dataLayers[idx]->_textureUnit>=0 )
	    setUpdateVar( _tilingInfo->_retilingNeeded, true );
    }
}


void LayeredTexture::setDataLayerFilterType( int id, FilterType filterType )
{
    const int idx = getDataLayerIndex( id );
    if ( idx!=-1 )
    {
	_dataLayers[idx]->_filterType = filterType;
	if ( _dataLayers[idx]->_textureUnit>=0 )
	    setUpdateVar( _tilingInfo->_retilingNeeded, true );
    }
}


void LayeredTexture::setDataLayerTextureUnit( int id, int unit )
{
    const int idx = getDataLayerIndex( id );
    if ( idx!=-1 )
	_dataLayers[idx]->_textureUnit = unit;
}


void LayeredTexture::setStackUndefLayerID( int id )
{
    raiseUndefChannelRefCount( false );
    setUpdateVar( _updateSetupStateSet, true );
    _stackUndefLayerId = id;
    raiseUndefChannelRefCount( true );
}


void LayeredTexture::setStackUndefChannel( int channel )
{
    if ( channel>=0 && channel<4 )
    {
	raiseUndefChannelRefCount( false );
	setUpdateVar( _updateSetupStateSet, true );
	_stackUndefChannel = channel;
	raiseUndefChannelRefCount( true );
    }
}


void LayeredTexture::setStackUndefColor( const osg::Vec4f& color )
{
    for ( int idx=0; idx<4; idx++ )
    {
	_stackUndefColor[idx] = color[idx]<=0.0f ? 0.0f :
				color[idx]>=1.0f ? 1.0f : color[idx];
    }
}


int LayeredTexture::getStackUndefLayerID() const
{ return _stackUndefLayerId; }


int LayeredTexture::getStackUndefChannel() const
{ return _stackUndefChannel; }


const osg::Vec4f& LayeredTexture::getStackUndefColor() const
{ return _stackUndefColor; }


#define GET_PROP( funcpostfix, type, variable, undefval ) \
type LayeredTexture::getDataLayer##funcpostfix( int id ) const \
{ \
    const int idx = getDataLayerIndex( id ); \
    static type undefvar = undefval; \
    return idx==-1 ? undefvar : _dataLayers[idx]->variable; \
}

GET_PROP( Image, const osg::Image*, _imageSource.get(), 0 )
GET_PROP( Origin, const osg::Vec2f&, _origin, osg::Vec2f(0.0f,0.0f) )
GET_PROP( TextureUnit, int, _textureUnit, -1 )
GET_PROP( Scale, const osg::Vec2f&, _scale, osg::Vec2f(1.0f,1.0f) )
GET_PROP( FilterType, FilterType, _filterType, Nearest )
GET_PROP( UndefChannel, int, _undefChannel, -1 )
GET_PROP( UndefLayerID, int, _undefLayerId, -1 )
GET_PROP( BorderColor, const osg::Vec4f&, _borderColor, osg::Vec4f(1.0f,1.0f,1.0f,1.0f) )
GET_PROP( ImageUndefColor, const osg::Vec4f&, _undefColor, osg::Vec4f(-1.0f,-1.0f,-1.0f,-1.0f) )


TransparencyType LayeredTexture::getDataLayerTransparencyType( int id, int channel ) const
{
    const int idx = getDataLayerIndex( id );
    if ( idx==-1 || channel<0 || channel>3 || !_dataLayers[idx]->_image )
	return FullyTransparent;

    TransparencyType& tt = _dataLayers[idx]->_transparency[channel];

    if ( tt==TransparencyUnknown )
	tt = getImageTransparencyType( _dataLayers[idx]->_image, channel );

    return addOpacity( tt, _dataLayers[idx]->_borderColor[channel] );
}


osg::Vec4f LayeredTexture::getDataLayerTextureVec( int id, const osg::Vec2f& globalCoord ) const
{
    const int idx = getDataLayerIndex( id );
    if ( idx==-1 )
	return osg::Vec4f( -1.0f, -1.0f, -1.0f, -1.0f );

    return _dataLayers[idx]->getTextureVec( globalCoord );
}


LayerProcess* LayeredTexture::getProcess( int idx )
{ return idx>=0 && idx<(int) _processes.size() ? _processes[idx] : 0;  }


const LayerProcess* LayeredTexture::getProcess( int idx ) const
{ return const_cast<LayeredTexture*>(this)->getProcess(idx); }


void LayeredTexture::addProcess( LayerProcess* process )
{
    if ( !process )
	return;

    process->ref();
    _lock.writeLock();
    _processes.push_back( process );
    setUpdateVar( _updateSetupStateSet, true );
    _lock.writeUnlock();
}


void LayeredTexture::removeProcess( const LayerProcess* process )
{
    _lock.writeLock();
    std::vector<LayerProcess*>::iterator it = std::find( _processes.begin(),
	    					    _processes.end(), process );
    if ( it!=_processes.end() )
    {
	process->unref();
	_processes.erase( it );
	setUpdateVar( _updateSetupStateSet, true );
    }

    _lock.writeUnlock();
}

#define MOVE_LAYER( func, cond, inc ) \
void LayeredTexture::func( const LayerProcess* process ) \
{ \
    _lock.writeLock(); \
    std::vector<LayerProcess*>::iterator it = std::find( _processes.begin(), \
	    					    _processes.end(), process);\
    if ( it!=_processes.end() ) \
    { \
	std::vector<LayerProcess*>::iterator neighbor = it inc; \
	if ( cond ) \
	{ \
	    std::swap( *it, *neighbor ); \
	    setUpdateVar( _updateSetupStateSet, true ); \
	} \
    } \
    _lock.writeUnlock(); \
}

MOVE_LAYER( moveProcessEarlier, it!=_processes.begin(), -1 )
MOVE_LAYER( moveProcessLater, neighbor!=_processes.end(), +1 )


void LayeredTexture::setGraphicsContextID( int id )
{
    _texInfo->_contextId = id;
    _texInfo->_isValid = false;
    _texInfo->_shadingSupport = false;
    updateTextureInfoIfNeeded();
}


int LayeredTexture::getGraphicsContextID() const
{ return _texInfo->_contextId; }


static int _maxTexSizeOverride = -1;
void LayeredTexture::overrideGraphicsContextMaxTextureSize( int maxTexSize )
{ _maxTexSizeOverride = maxTexSize; }


void LayeredTexture::updateTextureInfoIfNeeded() const
{
    if ( _texInfo->_isValid )
	return;

    // Shortcut to force continuous redraw until texture info is available
    const_cast<LayeredTexture*>(this)->triggerRedrawRequest();

    const int maxContextID = (int) osg::GraphicsContext::getMaxContextID();
    for( int contextID=0; contextID<=maxContextID; contextID++ )
    {
	if ( _texInfo->_contextId>=0 && _texInfo->_contextId!=contextID )
	    continue;

	const osg::VertexProgram::Extensions* vertExt = osg::VertexProgram::getExtensions( contextID, _texInfo->_contextId>=0 );
	const osg::FragmentProgram::Extensions* fragExt = osg::FragmentProgram::getExtensions( contextID, _texInfo->_contextId>=0 );

	const osg::Texture::Extensions* texExt = osg::Texture::getExtensions( contextID, _texInfo->_contextId>=0 );

	if ( !vertExt || !fragExt || !texExt )
	    continue;

#if OSG_VERSION_LESS_THAN(3,3,0)
	if ( !_texInfo->_isValid || _texInfo->_nrUnits>texExt->numTextureUnits() )
	    _texInfo->_nrUnits = texExt->numTextureUnits();
#endif

	if ( !_texInfo->_isValid || _texInfo->_maxSize>texExt->maxTextureSize() )
	{
	    _texInfo->_maxSize = texExt->maxTextureSize();
	    while ( _maxTexSizeOverride>0 && _texInfo->_maxSize>_maxTexSizeOverride && _texInfo->_maxSize>64 )
	    {
		_texInfo->_maxSize /= 2;
	    }
	}

	if ( !_texInfo->_isValid || _texInfo->_nonPowerOf2Support )
	    _texInfo->_nonPowerOf2Support = texExt->isNonPowerOfTwoTextureSupported( osg::Texture::LINEAR_MIPMAP_LINEAR );

	if ( !_texInfo->_isValid || _texInfo->_shadingSupport )
	    _texInfo->_shadingSupport = vertExt->isVertexProgramSupported() && fragExt->isFragmentProgramSupported() && _texInfo->_nrUnits>0;

	_texInfo->_isValid = true;
	_tilingInfo->_retilingNeeded = true;
    }
}


void LayeredTexture::updateTilingInfoIfNeeded() const
{
    if ( !_tilingInfo->_needsUpdate )
	return;

    _tilingInfo->reInit();

    std::vector<LayeredTextureData*>::const_iterator it = _dataLayers.begin();

    osg::Vec2f minBound( 0.0f, 0.0f );
    osg::Vec2f maxBound( 0.0f, 0.0f );
    osg::Vec2f minScale( 0.0f, 0.0f );
    osg::Vec2f minNoPow2Size( 0.0f, 0.0f );

    bool validLayerFound = false;

    for ( ; it!=_dataLayers.end(); it++ )
    {
	if ( !(*it)->_image.get() || (*it)->_id==_compositeLayerId )
	    continue;

	const osg::Vec2f scale( (*it)->_scale.x() * (*it)->_imageScale.x(),
				(*it)->_scale.y() * (*it)->_imageScale.y() );

	const osg::Vec2f layerSize( (*it)->_image->s() * scale.x(),
				    (*it)->_image->t() * scale.y() );

	const osg::Vec2f bound = layerSize + (*it)->_origin;

	if ( !validLayerFound || bound.x()>maxBound.x() )
	    maxBound.x() = bound.x();
	if ( !validLayerFound || bound.y()>maxBound.y() )
	    maxBound.y() = bound.y();
	if ( !validLayerFound || (*it)->_origin.x()<minBound.x() )
	    minBound.x() = (*it)->_origin.x();
	if ( !validLayerFound || (*it)->_origin.y()<minBound.y() )
	    minBound.y() = (*it)->_origin.y();
	if ( !validLayerFound || scale.x()<minScale.x() )
	    minScale.x() = scale.x();
	if ( !validLayerFound || scale.y()<minScale.y() )
	    minScale.y() = scale.y();

	if ( (minNoPow2Size.x()<=0.0f || layerSize.x()<minNoPow2Size.x()) &&
	     (*it)->_image->s() != powerOf2Ceil((*it)->_image->s()) )
	    minNoPow2Size.x() = layerSize.x();

	if ( (minNoPow2Size.y()<=0.0f || layerSize.y()<minNoPow2Size.y()) &&
	     (*it)->_image->t() != powerOf2Ceil((*it)->_image->t()) )
	    minNoPow2Size.y() = layerSize.y();

	validLayerFound = true;
    }

    if ( !validLayerFound )
	return;

    _tilingInfo->_envelopeSize = maxBound - minBound;
    _tilingInfo->_envelopeOrigin = minBound;
    _tilingInfo->_smallestScale = minScale;

    for ( int dim=0; dim<=1; dim++ )
    {
	_tilingInfo->_maxTileSize[dim] = ceil( _tilingInfo->_envelopeSize[dim]/ minScale[dim] );
	if ( minNoPow2Size[dim]>0.0f )
	    _tilingInfo->_maxTileSize[dim] = minNoPow2Size[dim] / minScale[dim];
    }
}


bool LayeredTexture::isDisplayFrozen() const
{
    std::vector<LayeredTextureData*>::const_iterator lit = _dataLayers.begin();
    for ( ; lit!=_dataLayers.end(); lit++ )
    {
	if ( (*lit)->_freezeDisplay )
	    return true;
    }

    return false;
}


bool LayeredTexture::needsRetiling() const
{
    if ( isDisplayFrozen() )
	return false;

    std::vector<LayeredTextureData*>::const_iterator lit = _dataLayers.begin();
    for ( ; lit!=_dataLayers.end(); lit++ )
	(*lit)->updateTileImagesIfNeeded();

    updateTilingInfoIfNeeded();
    updateTextureInfoIfNeeded();

    return _tilingInfo->_retilingNeeded || _retileCompositeLayer;
}


bool LayeredTexture::isEnvelopeDefined() const
{
    updateTilingInfoIfNeeded();
    return _tilingInfo->_envelopeSize[0]>0.0f && _tilingInfo->_envelopeSize[1]>0.0f;
}


osg::Vec2f LayeredTexture::imageEnvelopeSize() const
{
    updateTilingInfoIfNeeded();
    return _tilingInfo->_envelopeSize;
}


osg::Vec2f LayeredTexture::textureEnvelopeSize() const
{
    updateTilingInfoIfNeeded();
    return _tilingInfo->_envelopeSize - _tilingInfo->_smallestScale;
}


osg::Vec2f LayeredTexture::envelopeCenter() const
{
    updateTilingInfoIfNeeded();
    return _tilingInfo->_envelopeOrigin + textureEnvelopeSize()*0.5;
}


int LayeredTexture::maxTextureSize() const
{
    updateTextureInfoIfNeeded();
    return _texInfo->_isValid ? _texInfo->_maxSize : -1;
}


int LayeredTexture::nrTextureUnits() const
{
    updateTextureInfoIfNeeded();
    return _texInfo->_isValid ? _texInfo->_nrUnits : -1;
}


void LayeredTexture::reInitTiling( float texelSizeRatio )
{
    _reInitTiling = true;
    updateTilingInfoIfNeeded();
    updateTextureInfoIfNeeded();
    assignTextureUnits();

    std::vector<LayeredTextureData*>::iterator lit = _dataLayers.begin();
    for ( ; lit!=_dataLayers.end(); lit++ )
	(*lit)->cleanUp();

    setUpdateVar( _tilingInfo->_retilingNeeded, false );
    _externalTexelSizeRatio = texelSizeRatio; 
    _reInitTiling = false;
}


void LayeredTexture::setSeamPower( int power, int dim )
{
    if ( dim!=1 )
	_seamPower.x() = power;
    if ( dim!=0 )
	_seamPower.y() = power;

    setUpdateVar( _tilingInfo->_retilingNeeded, true );
}


int LayeredTexture::getSeamPower( int dim ) const
{ return dim>0 ? _seamPower.y() : _seamPower.x(); }


int LayeredTexture::getSeamWidth( int layerIdx, int dim ) const
{
    if ( layerIdx<0 || layerIdx>=nrDataLayers() || dim<0 || dim>1 )
	return 1;

    float ratio = 1.0f;
    if ( _externalTexelSizeRatio )
	ratio = fabs( _externalTexelSizeRatio );
    if ( dim==1 )
	ratio = 1.0f / ratio;

    LayeredTextureData* layer = _dataLayers[layerIdx];
    ratio *= _tilingInfo->_smallestScale[dim] / layer->_scale[1-dim];

    int seamWidth = (int) floor( ratio+0.5 );

    if ( seamWidth>_texInfo->_maxSize/4 )
	seamWidth = _texInfo->_maxSize/4;
    if ( seamWidth<1 )
	seamWidth = 1;

    seamWidth = powerOf2Ceil( seamWidth );

    int seamPower = getSeamPower( dim );
    while ( (--seamPower)>=0 && seamWidth<_texInfo->_maxSize/4 )
	seamWidth *= 2;

    return seamWidth;
}


int LayeredTexture::getTileOverlapUpperBound( int dim ) const
{
     if ( dim<0 || dim>1 )
	 return 0;

    float maxScaledWidth = 1.0f;

    for ( int idx=0; idx<nrDataLayers(); idx++ )
    {
	LayeredTextureData* layer = _dataLayers[idx];
	if ( !layer->_image.get() || layer->_id==_compositeLayerId )
	    continue;

	const float scaledWidth = layer->_scale[dim] * getSeamWidth(idx,dim);
	if ( scaledWidth > maxScaledWidth )
	    maxScaledWidth = scaledWidth;
    }

    // Include one extra seam width in upper bound to cover seam alignment
    return 3 * (int) ceil(maxScaledWidth/_tilingInfo->_smallestScale[dim]);
}


bool LayeredTexture::planTiling( unsigned short brickSize, std::vector<float>& xTickMarks, std::vector<float>& yTickMarks, bool strict ) const
{
    const Vec2i requestedSize( brickSize, brickSize );
    Vec2i actualSize = requestedSize;

    if ( !strict && _textureSizePolicy!=AnySize )
    {
	const osg::Vec2f& maxTileSize = _tilingInfo->_maxTileSize;

	for ( int dim=0; dim<=1; dim++ )
	{
	    const int overlap = getTileOverlapUpperBound( dim ); 
	    actualSize[dim] = powerOf2Ceil( brickSize+overlap );

	    // To minimize absolute difference with requested brick size
	    if ( brickSize<0.75*actualSize[dim]-overlap )
	    {
		// But let's stay above half the requested brick size
		if ( brickSize<actualSize[dim]-2*overlap )
		    actualSize[dim] /= 2;
	    }

	    bool hadToReduceTileSize = false;
	    while ( actualSize[dim]>maxTileSize[dim] && maxTileSize[dim]>0.0f )
	    {
		hadToReduceTileSize = true;
		actualSize[dim] /= 2;
	    }

	    if ( hadToReduceTileSize && overlap>0.75*actualSize[dim] )
	    {
		// Cut down seam if it (almost) overgrows the tile
		actualSize[dim] /= 4;
		if ( actualSize[dim]>brickSize )
		    actualSize[dim] = brickSize;
	    }
	    else 
		actualSize[dim] -= overlap;

	    // std::cout << "Tile size: " << actualSize[dim] << ", overlap: " << overlap << std::endl;
	}
    }

    const osg::Vec2f& size = _tilingInfo->_envelopeSize;
    const osg::Vec2f& minScale = _tilingInfo->_smallestScale;

    bool xRes = divideAxis( size.x()/minScale.x(), actualSize.x(), xTickMarks );
    bool yRes = divideAxis( size.y()/minScale.y(), actualSize.y(), yTickMarks );

    return xRes && yRes && actualSize==requestedSize;
}


bool LayeredTexture::divideAxis( float totalSize, int brickSize,
				 std::vector<float>& tickMarks ) const
{
    tickMarks.push_back( 0.0f );

    if ( totalSize <= 1.0f ) 
    {
	// to display something if no layers or images defined yet
	tickMarks.push_back( 1.0f );
	return false;
    }

    int stepSize = _texInfo->_maxSize;
    if ( brickSize < stepSize )
	stepSize = brickSize<1 ? 1 : brickSize;

    for ( float fidx=stepSize; fidx+EPS<totalSize-1.0f; fidx += stepSize )
	tickMarks.push_back( fidx );

    tickMarks.push_back( totalSize-1.0f );

    return stepSize==brickSize;
}


osg::Vec2 LayeredTexture::tilingPlanResolution() const
{
    return osg::Vec2( 1.0f / _tilingInfo->_smallestScale[0],
		      1.0f / _tilingInfo->_smallestScale[1] );
}


static void boundedCopy( unsigned char* dest, const unsigned char* src, int len, const unsigned char* lowPtr, const unsigned char* highPtr )
{
    if ( src>=highPtr || src+len<=lowPtr )
    {
	std::cerr << "Unsafe memcpy" << std::endl;
	return;
    }

    if ( src < lowPtr )
    {
	std::cerr << "Unsafe memcpy" << std::endl;
	len -= lowPtr - src;
	src = lowPtr;
    }
    if ( src+len > highPtr )
    {
	std::cerr << "Unsafe memcpy" << std::endl;
	len = highPtr - src;
    }

    memcpy( dest, src, len );
}


static void copyImageWithStride( const unsigned char* srcImage, unsigned char* tileImage, int nrRows, int rowSize, int offset, int stride, int pixelSize, const unsigned char* srcEnd )
{
    int rowLen = rowSize*pixelSize;
    const unsigned char* srcPtr = srcImage+offset;

    if ( !stride )
    {
	boundedCopy( tileImage, srcPtr, rowLen*nrRows, srcImage, srcEnd);
	return;
    }

    const int srcInc = (rowSize+stride)*pixelSize;
    for ( int idx=0; idx<nrRows; idx++, srcPtr+=srcInc, tileImage+=rowLen )
	boundedCopy( tileImage, srcPtr, rowLen, srcImage, srcEnd );
}


static void copyImageTile( const osg::Image& srcImage, osg::Image& tileImage, const Vec2i& tileOrigin, const Vec2i& tileSize )
{
    tileImage.allocateImage( tileSize.x(), tileSize.y(), srcImage.r(), srcImage.getPixelFormat(), srcImage.getDataType(), srcImage.getPacking() );

    const int pixelSize = srcImage.getPixelSizeInBits()/8;
    int offset = tileOrigin.y()*srcImage.s()+tileOrigin.x();
    offset *= pixelSize;
    const int stride = srcImage.s()-tileSize.x();
    const unsigned char* sourceEnd = srcImage.data() + srcImage.s()*srcImage.t()*srcImage.r()*pixelSize;  

    copyImageWithStride( srcImage.data(), tileImage.data(), tileSize.y(), tileSize.x(), offset, stride, pixelSize, sourceEnd );
}


osg::StateSet* LayeredTexture::createCutoutStateSet(const osg::Vec2f& origin, const osg::Vec2f& opposite, std::vector<LayeredTexture::TextureCoordData>& tcData ) const
{
    tcData.clear();
    osg::ref_ptr<osg::StateSet> stateset = new osg::StateSet;

    const osg::Vec2f smallestScale = _tilingInfo->_smallestScale;
    osg::Vec2f globalOrigin( smallestScale.x() * (origin.x()+0.5),
			     smallestScale.y() * (origin.y()+0.5) );
    globalOrigin += _tilingInfo->_envelopeOrigin;

    osg::Vec2f globalOpposite( smallestScale.x() * (opposite.x()+0.5),
			       smallestScale.y() * (opposite.y()+0.5) );
    globalOpposite += _tilingInfo->_envelopeOrigin;

    for ( int idx=nrDataLayers()-1; idx>=0; idx-- )
    {
	LayeredTextureData* layer = _dataLayers[idx];
	if ( layer->_textureUnit<0 )
	    continue;

	const osg::Vec2f localOrigin = layer->getLayerCoord( globalOrigin );
	const osg::Vec2f localOpposite = layer->getLayerCoord( globalOpposite );

	const osg::Image* srcImage = layer->_image;
	if ( !srcImage || !srcImage->s() || !srcImage->t() )
	    continue;

	const Vec2i imageSize( srcImage->s(), srcImage->t() );
	Vec2i hasBorderArea, tileOrigin, tileSize;

	bool overflowErrorMsg = false;
	bool resizeHint = false;

	for ( int dim=0; dim<=1; dim++ )
	{
	    hasBorderArea[dim] = localOrigin[dim]<-EPS || localOpposite[dim]>imageSize[dim]+EPS;

	    if ( localOpposite[dim]<EPS || localOrigin[dim]>imageSize[dim]-EPS )
	    {
		tileOrigin[dim] = localOpposite[dim]<EPS ? 0 : imageSize[dim]-1;
		tileSize[dim] = 1;  // More needed only if mipmapping-induced
		continue;	    // artifacts in extended-edge-pixel borders
	    }			    // become an issue. 

	    const int orgSeamWidth = getSeamWidth( idx, dim );
	    for ( int width=orgSeamWidth; ; width/=2 )
	    {
		tileOrigin[dim] = (int) floor( localOrigin[dim]-0.5 );
		int tileOpposite = (int) ceil( localOpposite[dim]+0.5 );

		/* width==0 represents going back to original seam width
		   after anything smaller does not solve the puzzle either. */ 
		const int seamWidth = width ? width : orgSeamWidth;

		tileOrigin[dim] -= seamWidth/2;
		if ( tileOrigin[dim]<=0 )
		    tileOrigin[dim] = 0;
		else 
		    // Align seams of subsequent tiles to minimize artifacts
		    tileOrigin[dim] -= tileOrigin[dim]%seamWidth; 

		tileOpposite += ((3*seamWidth)/2) - 1;
		tileOpposite -= tileOpposite%seamWidth;

		if ( tileOpposite>imageSize[dim] )  // Cannot guarantee seam
		    tileOpposite = imageSize[dim];  // alignment at last tile

		tileSize[dim] = tileOpposite - tileOrigin[dim];
		if ( tileSize[dim]>_texInfo->_maxSize )
		{
		    if ( seamWidth>1 )
			continue;

		    if ( !overflowErrorMsg )
		    {
			overflowErrorMsg = true;
			std::cerr << "Cut-out exceeds maximum texture size: " << _texInfo->_maxSize << std::endl;
		    }

		    tileSize[dim] = _texInfo->_maxSize;
		    hasBorderArea[dim] = true;
		    break;
		}

		if ( seamWidth==orgSeamWidth && usedTextureSizePolicy()==AnySize )
		    /* Note that seam alignment will be broken if OpenGL
		       implementation of AnySize is going to resample. */
		    break;

		const int powerOf2Size = powerOf2Ceil( tileSize[dim] );

		if ( powerOf2Size>imageSize[dim] )
		{
		    if ( orgSeamWidth>1 && width>0 )
			continue;

		    if ( !resizeHint ) 
		    {
			resizeHint = true;
			if ( _textureSizePolicy!=AnySize )
			{
			    std::cerr << "Can't avoid texture resampling for this cut-out: increase MaxTextureCopySize" << std::endl ;
			}
		    }
		    break;
		}

		const int extraSeam = (powerOf2Size-tileSize[dim]) / 2;
		// Be careful not to break the current seam alignments
		tileOrigin[dim] -= seamWidth * (extraSeam/seamWidth);

		tileSize[dim] = powerOf2Size;

		if ( tileOrigin[dim]<0 )
		    tileOrigin[dim] = 0;

		if ( tileOrigin[dim]+tileSize[dim] > imageSize[dim] )
		    tileOrigin[dim] = imageSize[dim] - tileSize[dim];

		break;
	    }
	}

	osg::ref_ptr<osg::Image> tileImage = new osg::Image;

#ifdef USE_IMAGE_STRIDE
	if ( !resizeHint ) // OpenGL crashes when resizing image with stride
	{
	    osg::ref_ptr<osg::Image> si = const_cast<osg::Image*>(srcImage);
	    tileImage->setUserData( si.get() );
	    tileImage->setImage( tileSize.x(), tileSize.y(), si->r(), si->getInternalTextureFormat(), si->getPixelFormat(), si->getDataType(), si->data(tileOrigin.x(),tileOrigin.y()), osg::Image::NO_DELETE, si->getPacking(), si->s() ); 

	    tileImage->ref();
	    const_cast<LayeredTexture*>(this)->_lock.writeLock();
	    layer->_tileImages.push_back( tileImage );
	    const_cast<LayeredTexture*>(this)->_lock.writeUnlock();
	}
	else
#endif
	    copyImageTile( *srcImage, *tileImage, tileOrigin, tileSize );

	osg::Texture::WrapMode xWrapMode = osg::Texture::CLAMP_TO_EDGE;
	if ( layer->_borderColor[0]>=0.0f && hasBorderArea.x() )
	    xWrapMode = osg::Texture::CLAMP_TO_BORDER;

	osg::Texture::WrapMode yWrapMode = osg::Texture::CLAMP_TO_EDGE;
	if ( layer->_borderColor[0]>=0.0f && hasBorderArea.y() )
	    yWrapMode = osg::Texture::CLAMP_TO_BORDER;

	osg::Vec2f tc00, tc01, tc10, tc11;
	tc00.x() = (localOrigin.x() - tileOrigin.x()) / tileSize.x();
	tc00.y() = (localOrigin.y() - tileOrigin.y()) / tileSize.y();
	tc11.x() = (localOpposite.x()-tileOrigin.x()) / tileSize.x();
	tc11.y() = (localOpposite.y()-tileOrigin.y()) / tileSize.y();
	tc01 = osg::Vec2f( tc11.x(), tc00.y() );
	tc10 = osg::Vec2f( tc00.x(), tc11.y() );

	tcData.push_back( TextureCoordData( layer->_textureUnit, tc00, tc01, tc10, tc11 ) );

	osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D( tileImage.get() );
	texture->setResizeNonPowerOfTwoHint( resizeHint );
	texture->setWrap( osg::Texture::WRAP_S, xWrapMode );
	texture->setWrap( osg::Texture::WRAP_T, yWrapMode );

	osg::Texture::FilterMode filterMode = layer->_filterType==Nearest ? osg::Texture::NEAREST : osg::Texture::LINEAR;
	texture->setFilter( osg::Texture::MAG_FILTER, filterMode );

	filterMode = layer->_filterType==Nearest ? osg::Texture::NEAREST_MIPMAP_NEAREST : osg::Texture::LINEAR_MIPMAP_LINEAR;
	texture->setFilter( osg::Texture::MIN_FILTER, filterMode );

	texture->setBorderColor( layer->_borderColor );

	stateset->setTextureAttributeAndModes( layer->_textureUnit, texture.get() );
    }

    return stateset.release();
}


void LayeredTexture::updateSetupStateSet()
{
    setUpdateVar( _updateSetupStateSet, true );
}


osg::StateSet* LayeredTexture::getSetupStateSet()
{
    updateSetupStateSetIfNeeded();
    return _setupStateSet;
}


void LayeredTexture::updateSetupStateSetIfNeeded()
{
    if ( isDisplayFrozen() )
	return;

    _lock.readLock();

    if ( !_setupStateSet )
    {
	_setupStateSet = new osg::StateSet;
	setUpdateVar( _updateSetupStateSet, true );
	setRenderingHint( false );
    }

    checkForModifiedImages();

    std::vector<LayerProcess*>::iterator it = _processes.begin();
    for ( ; _isOn && it!=_processes.end(); it++ )
	(*it)->checkForModifiedColorSequence();

    if ( _updateSetupStateSet )
    {
	_compositeLayerUpdate = !_retileCompositeLayer;
	buildShaders();
	setUpdateVar( _updateSetupStateSet, false );
    }

    _lock.readUnlock();
}


void LayeredTexture::checkForModifiedImages()
{
    std::vector<LayeredTextureData*>::iterator it = _dataLayers.begin();
    for ( ; it!=_dataLayers.end(); it++ )
    {
	(*it)->_imageModifiedFlag = false;
	if ( (*it)->_imageSource.get() )
	{
	    const int modifiedCount = (*it)->_imageSource->getModifiedCount();
	    if ( modifiedCount!=(*it)->_imageModifiedCount )
	    {
		setDataLayerImage( (*it)->_id, (*it)->_imageSource );
		(*it)->_imageModifiedFlag = true;
		setUpdateVar( _updateSetupStateSet, true );
	    }
	}
    }

    for ( it=_dataLayers.begin(); it!=_dataLayers.end(); it++ )
    {
	const int udfIdx = getDataLayerIndex( (*it)->_undefLayerId );
	if ( udfIdx!=-1 && _dataLayers[udfIdx]->_imageModifiedFlag )
	    (*it)->clearTransparencyType();
    }
}


void LayeredTexture::buildShaders()
{
    _useShaders = _allowShaders && _texInfo->_shadingSupport;

    int nrProc = 0;
    int nrUsedLayers = 0;

    std::vector<int> orderedLayerIDs;
    bool stackIsOpaque = false;

    if ( _useShaders )
    {
	nrProc = getProcessInfo( orderedLayerIDs, nrUsedLayers, _useShaders, &stackIsOpaque );
    }

    if ( !_useShaders )
    {
	const bool create = !_retileCompositeLayer;
	setUpdateVar( _retileCompositeLayer, false );

	if ( create )
	    createCompositeTexture( !_texInfo->_isValid );

	if ( !_retileCompositeLayer )
	{
	    _setupStateSet->clear();
	    setRenderingHint( getDataLayerTransparencyType(_compositeLayerId)==Opaque );
	}

	return;
    }

    bool needColSeqTexture = false;
    int minUnit = _texInfo->_nrUnits;
    std::vector<int> activeUnits;

    std::vector<int>::iterator it = orderedLayerIDs.begin();
    for ( ; nrUsedLayers>0; it++, nrUsedLayers-- )
    {
	if ( *it )
	{
	    const int unit = getDataLayerTextureUnit( *it );
	    activeUnits.push_back( unit );
	    if ( unit<minUnit )
		minUnit = unit;
	}
	else
	    needColSeqTexture = true;
    }

    if ( minUnit<0 || (minUnit==0 && needColSeqTexture) )
    {
	setUpdateVar( _tilingInfo->_retilingNeeded, true );
	return;
    }

    _setupStateSet->clear();

    std::string code;
    getVertexShaderCode( code, activeUnits );
    osg::ref_ptr<osg::Shader> vertexShader = new osg::Shader( osg::Shader::VERTEX, code );

    if ( needColSeqTexture )
    {
	createColSeqTexture();
	activeUnits.push_back( 0 );
    }

    getFragmentShaderCode( code, activeUnits, nrProc, stackIsOpaque );
    osg::ref_ptr<osg::Shader> fragmentShader = new osg::Shader( osg::Shader::FRAGMENT, code );

    osg::ref_ptr<osg::Program> program = new osg::Program;

    program->addShader( vertexShader.get() );
    program->addShader( fragmentShader.get() );
    _setupStateSet->setAttributeAndModes( program.get() );

    char samplerName[20];
    for ( it=activeUnits.begin(); it!=activeUnits.end(); it++ )
    {
	sprintf( samplerName, "texture%d", *it );
	_setupStateSet->addUniform( new osg::Uniform(samplerName, *it) );
    }

    setRenderingHint( stackIsOpaque );
}


void LayeredTexture::setRenderingHint( bool stackIsOpaque )
{
    if ( isDataLayerOK(_stackUndefLayerId) && _stackUndefColor[3]<1.0f )
	stackIsOpaque = false;

    if ( !stackIsOpaque ) 
    {
	osg::ref_ptr<osg::BlendFunc> blendFunc = new osg::BlendFunc;
	blendFunc->setFunction( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	_setupStateSet->setAttributeAndModes( blendFunc );
	_setupStateSet->setRenderingHint( osg::StateSet::TRANSPARENT_BIN );
    }
    else
	_setupStateSet->setRenderingHint( osg::StateSet::OPAQUE_BIN );
}


int LayeredTexture::getProcessInfo( std::vector<int>& layerIDs, int& nrUsedLayers, bool& useShaders, bool* stackIsOpaque ) const
{
    layerIDs.empty();
    std::vector<int> skippedIDs;
    int nrProc = 0;
    nrUsedLayers = -1;

    if ( stackIsOpaque )
	*stackIsOpaque = false;

    if ( isDataLayerOK(_stackUndefLayerId) )
	layerIDs.push_back( _stackUndefLayerId );
    else if ( _stackUndefLayerId>0 )
	skippedIDs.push_back( _stackUndefLayerId );

    std::vector<LayerProcess*>::const_reverse_iterator it = _processes.rbegin();
    for ( ; _isOn && it!=_processes.rend(); it++ )
    {
	const TransparencyType transparency = (*it)->getTransparencyType();
	int nrPushed = 0;

	for ( int idx=-1; ; idx++ )
	{
	    bool skip = transparency==FullyTransparent;
	    int id = -1;

	    if ( idx>=0 )
	    {
		id = (*it)->getDataLayerID( idx/2 );
		if ( !(*it)->isOn(idx/2) || !isDataLayerOK(id) )
		    skip = true;

		if ( idx%2 )
		{
		    id = getDataLayerUndefLayerID(id);
		    if ( !isDataLayerOK(id) )
			skip = true;
		}
		else if ( idx>=8 && id<0 )
		    break;
	    }
	    else if ( (*it)->needsColorSequence() )
		id = 0;		// ColSeqTexture represented by ID=0

	    if ( id<0 )
		continue;

	    const std::vector<int>::iterator it1 = std::find(layerIDs.begin(),layerIDs.end(),id);
	    const std::vector<int>::iterator it2 = std::find(skippedIDs.begin(),skippedIDs.end(),id);

	    if ( nrUsedLayers<0 )
	    {
		if ( !skip )
		{
		    if ( it2!=skippedIDs.end() )
			skippedIDs.erase( it2 );
		}
		else if ( it1==layerIDs.end() && it2==skippedIDs.end() )
		    skippedIDs.push_back( id );
	    }

	    if ( it1==layerIDs.end() )
	    {
		if ( nrUsedLayers<0 )
		{
		    if ( !skip )
		    {
			layerIDs.push_back( id );
			nrPushed++;
		    }
		}
		else if ( it2==skippedIDs.end() )
		    layerIDs.push_back( id );
	    }
	}

	if ( nrUsedLayers<0 )
	{
	    const int sz = layerIDs.size();
	    if ( sz > _texInfo->_nrUnits )
	    {
		nrUsedLayers = sz-nrPushed;
		if ( !nrProc || !_maySkipEarlyProcesses )
		    useShaders = false;
	    }
	    else
	    {
		nrProc++;
		if ( transparency==Opaque )
		{
		    nrUsedLayers = sz;
		    if ( stackIsOpaque )
			*stackIsOpaque = true;
		}
	    }
	}
    }

    if ( nrUsedLayers<0 )
	nrUsedLayers = layerIDs.size();

    layerIDs.insert( layerIDs.begin()+nrUsedLayers,
		     skippedIDs.begin(), skippedIDs.end() );
    return nrProc;
}


void LayeredTexture::createColSeqTexture()
{
    osg::ref_ptr<osg::Image> colSeqImage = new osg::Image();
    const int nrProc = nrProcesses();
    const int texSize = powerOf2Ceil( nrProc );
    colSeqImage->allocateImage( 256, texSize, 1, GL_RGBA, GL_UNSIGNED_BYTE );

    const int rowSize = colSeqImage->getRowSizeInBytes();
    std::vector<LayerProcess*>::const_iterator it = _processes.begin();
    for ( int idx=0; _isOn && idx<nrProc; idx++, it++ )
    {
	const unsigned char* ptr = (*it)->getColorSequencePtr();
	if ( ptr )
	    memcpy( colSeqImage->data(0,idx), ptr, rowSize );

	(*it)->setColorSequenceTextureCoord( (idx+0.5)/texSize );
    }
    osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D( colSeqImage );
    texture->setFilter( osg::Texture::MIN_FILTER, osg::Texture::NEAREST );
    texture->setFilter( osg::Texture::MAG_FILTER, osg::Texture::NEAREST );
    _setupStateSet->setTextureAttributeAndModes( 0, texture.get() ); 
}


void LayeredTexture::assignTextureUnits()
{
    _useShaders = _allowShaders && _texInfo->_shadingSupport;

    std::vector<int> orderedLayerIDs;
    int nrUsedLayers = 0;

    if ( _useShaders )
	getProcessInfo( orderedLayerIDs, nrUsedLayers, _useShaders );

    std::vector<LayeredTextureData*>::iterator lit = _dataLayers.begin();
    for ( ; lit!=_dataLayers.end(); lit++ )
	(*lit)->_textureUnit = -1;

    if ( _useShaders )
    {
	int unit = 0;	// Reserved for ColSeqTexture if needed

	// nrUsedLayers = _texInfo->_nrUnits;
	// preloading shows bad performance in case of many tiles!

	std::vector<int>::iterator iit = orderedLayerIDs.begin();
	for ( ; iit!=orderedLayerIDs.end() && nrUsedLayers>0; iit++ )
	{
	    if ( (*iit)>0 )
		setDataLayerTextureUnit( *iit, (++unit)%_texInfo->_nrUnits );

	    nrUsedLayers--;
	}
    }
    else
	setDataLayerTextureUnit( _compositeLayerId, 0 );

    if ( _tilingInfo->_retilingNeeded || _updateSetupStateSet )
	setUpdateVar( _retileCompositeLayer, false );

    setUpdateVar( _updateSetupStateSet, true );
    updateSetupStateSetIfNeeded();
}


void LayeredTexture::getVertexShaderCode( std::string& code, const std::vector<int>& activeUnits ) const
{
    code =

"void main(void)\n"
"{\n"
"    vec3 fragNormal = normalize(gl_NormalMatrix * gl_Normal);\n"
"\n"
"    vec4 diffuse = vec4(0.0,0.0,0.0,0.0);\n"
"    vec4 ambient = vec4(0.0,0.0,0.0,0.0);\n"
"    vec4 specular = vec4(0.0,0.0,0.0,0.0);\n"
"\n"
"    for ( int light=0; light<2; light++ )\n"
"    {\n"
"        vec3 lightDir = normalize( vec3(gl_LightSource[light].position) );\n"
"        float NdotL = abs( dot(fragNormal, lightDir) );\n"
"\n"
"        diffuse += gl_LightSource[light].diffuse * NdotL;\n"
"        ambient += gl_LightSource[light].ambient;\n"
"        float pf = 0.0;\n"
"        if (NdotL != 0.0)\n"
"        {\n"
"            float nDotHV = abs( \n"
"	          dot(fragNormal, vec3(gl_LightSource[light].halfVector)) );\n"
"            pf = pow( nDotHV, gl_FrontMaterial.shininess );\n"
"        }\n"
"        specular += gl_LightSource[light].specular * pf;\n"
"    }\n"
"\n"
"    gl_FrontColor =\n"
"        gl_FrontLightModelProduct.sceneColor +\n"
"        ambient  * gl_FrontMaterial.ambient +\n"
"        diffuse  * gl_FrontMaterial.diffuse +\n"
"        specular * gl_FrontMaterial.specular;\n"
"\n"
"    gl_Position = ftransform();\n"
"\n";

    char line[100];
    std::vector<int>::const_iterator it = activeUnits.begin();
    for ( ; it!=activeUnits.end(); it++ )
    {
	sprintf( line, "    gl_TexCoord[%d] = gl_TextureMatrix[%d] * gl_MultiTexCoord%d;\n", *it, *it, *it );
	code += line;
    }

    code += "}\n";

    //std::cout << code << std::endl;
}


void LayeredTexture::getFragmentShaderCode( std::string& code, const std::vector<int>& activeUnits, int nrProc, bool stackIsOpaque ) const
{
    code.clear();
    char line[100];
    std::vector<int>::const_iterator iit = activeUnits.begin();
    for ( ; iit!=activeUnits.end(); iit++ )
    {
	sprintf( line, "uniform sampler2D texture%d;\n", *iit );
	code += line;
    }

    code += "\n";
    const bool stackUdf = isDataLayerOK(_stackUndefLayerId);
    code += stackUdf ? "void process( float stackudf )\n" :
		       "void process( void )\n";
    code += "{\n"
	    "    vec4 col, udfcol;\n"
	    "    vec2 texcrd;\n"
	    "    float a, b, udf, oldudf, orgcol3;\n"
	    "\n";

    int stage = 0;
    float minOpacity = 1.0f;

    std::vector<LayerProcess*>::const_reverse_iterator it = _processes.rbegin();
    for ( ; _isOn && it!=_processes.rend() && nrProc--; it++ )
    {
	if ( (*it)->getOpacity() < minOpacity )
	    minOpacity = (*it)->getOpacity();

	if ( (*it)->getTransparencyType()==FullyTransparent )
	    continue;

	if ( stage )
	{
	    code += "\n"
		    "    if ( gl_FragColor.a >= 1.0 )\n"
		    "       return;\n"
		    "\n";
	}

	(*it)->getShaderCode( code, stage++ );
    }

    if ( !stage )
    {
	sprintf( line, "    gl_FragColor = vec4(1.0,1.0,1.0,%.6f);\n", minOpacity );
	code += line;
    }

    code += "}\n"
	    "\n"
	    "void main( void )\n"
	    "{\n"
	    "    if ( gl_FrontMaterial.diffuse.a <= 0.0 )\n"
	    "        discard;\n"
	    "\n";

    if ( stackUdf )
    {
	const int udfUnit = getDataLayerTextureUnit( _stackUndefLayerId );
	sprintf( line, "    vec2 texcrd = gl_TexCoord[%d].st;\n", udfUnit );
	code += line;
	sprintf( line, "    float udf = texture2D( texture%d, texcrd )[%d];\n", udfUnit, _stackUndefChannel );
	code += line;
	code += "\n"
		"    if ( udf < 1.0 )\n"
		"        process( udf );\n"
		"\n";

	sprintf( line, "    vec4 udfcol = vec4(%.6f,%.6f,%.6f,%.6f);\n", _stackUndefColor[0], _stackUndefColor[1], _stackUndefColor[2], _stackUndefColor[3] );
	code += line;

	code += "\n"
		"    if ( udf >= 1.0 )\n"
	    	"        gl_FragColor = udfcol;\n"
		"    else if ( udf > 0.0 )\n";

	if ( _stackUndefColor[3]<=0.0f )
	    code += "        gl_FragColor.a *= 1.0-udf;\n";
	else if ( _stackUndefColor[3]>=1.0f && stackIsOpaque )
	    code += "        gl_FragColor = mix( gl_FragColor, udfcol, udf );\n";
	else
	    code += "    {\n"
		    "        if ( gl_FragColor.a > 0.0 )\n"
		    "        {\n"
		    "            vec4 col = gl_FragColor;\n"
		    "            gl_FragColor.a = mix( col.a, udfcol.a, udf );\n"
		    "            col.rgb = mix( col.a*col.rgb, udfcol.a*udfcol.rgb, udf );\n"
		    "            gl_FragColor.rgb = col.rgb / gl_FragColor.a;\n"
		    "        }\n"
		    "        else\n"
		    "            gl_FragColor = vec4( udfcol.rgb, udf*udfcol.a );\n"
		    "    }\n";
    }
    else
	 code += "    process();\n";

    code += "\n"
	    "    gl_FragColor.a *= gl_FrontMaterial.diffuse.a;\n"
	    "    gl_FragColor.rgb *= gl_Color.rgb;\n"
	    "}\n";

    //std::cout << code << std::endl;
}


void LayeredTexture::allowShaders( bool yn, bool maySkipEarlyProcs )
{
    if ( _allowShaders!=yn || _maySkipEarlyProcesses!=maySkipEarlyProcs )
    {
	_allowShaders = yn;
	_maySkipEarlyProcesses = maySkipEarlyProcs;
	setUpdateVar( _tilingInfo->_retilingNeeded, true );
    }
}


void LayeredTexture::setTextureSizePolicy( TextureSizePolicy policy )
{
    _textureSizePolicy = policy;
    setUpdateVar( _tilingInfo->_retilingNeeded, true );
}


LayeredTexture::TextureSizePolicy LayeredTexture::getTextureSizePolicy() const
{ return _textureSizePolicy; }


LayeredTexture::TextureSizePolicy LayeredTexture::usedTextureSizePolicy() const
{
    if ( _textureSizePolicy==AnySize && !_texInfo->_nonPowerOf2Support )
	return PowerOf2;

    return _textureSizePolicy;
}


void LayeredTexture::setMaxTextureCopySize( unsigned int width_x_height )
{
    _maxTextureCopySize = width_x_height;

    std::vector<LayeredTextureData*>::iterator it = _dataLayers.begin();
    for ( ; it!=_dataLayers.end(); it++ )
    {
	(*it)->_imageModifiedFlag = false;
	if ( (*it)->_image.get()!=(*it)->_imageSource.get() )
	    (*it)->_imageModifiedCount = -1;
    }
}


//============================================================================


class CompositeTextureTask : public osg::Referenced, public OpenThreads::Thread
{
public:
			CompositeTextureTask(const LayeredTexture& lt,
				    osg::Image& image,osg::Vec4f& borderCol,
				    const std::vector<LayerProcess*>& procs,
				    float minOpacity,bool dummyTexture,
				    int startNr,int stopNr,
				    OpenThreads::BlockCount& ready)
			    : _lt( lt )
			    , _image( image )
			    , _borderColor( borderCol )
			    , _processList( procs )
			    , _minOpacity( minOpacity )
			    , _dummyTexture( dummyTexture )
			    , _start( startNr>=0 ? startNr : 0 )
			    , _stop( stopNr<=image.s()*image.t() ? stopNr : image.s()*image.t() )
			    , _readyCount( ready )
			{}

			~CompositeTextureTask()
			{
			    while( isRunning() )
				OpenThreads::Thread::YieldCurrentThread();
			}

    void		run();

protected:

    const LayeredTexture&		_lt;
    bool				_dummyTexture;
    osg::Image&				_image;
    osg::Vec4f&				_borderColor;
    const std::vector<LayerProcess*>&	_processList;
    float				_minOpacity;
    int					_start;
    int					_stop;
    OpenThreads::BlockCount&		_readyCount;
};


void CompositeTextureTask::run()
{
    const int idx = _lt.getDataLayerIndex( _lt._compositeLayerId );
    const osg::Vec2f& origin = _lt._dataLayers[idx]->_origin;
    const osg::Vec2f& scale = _lt._dataLayers[idx]->_scale;

    const LayeredTextureData* udfLayer = 0;
    const int udfIdx = _lt.getDataLayerIndex( _lt._stackUndefLayerId );
    if ( udfIdx>=0 )
	udfLayer = _lt._dataLayers[udfIdx];

    const osg::Vec4f& udfColor = _lt._stackUndefColor;
    const int udfChannel = _lt._stackUndefChannel;
    float udf = 0.0f;

    std::vector<LayerProcess*>::const_reverse_iterator it;
    unsigned char* imagePtr = _image.data() + _start*4;
    const int width = _image.s();
    const int nrImagePixels = width * _image.t();

    for ( int pixelNr=_start; pixelNr<=_stop; pixelNr++ ) 
    {
	osg::Vec2f globalCoord( origin.x()+scale.x()*(pixelNr%width+0.5),
				origin.y()+scale.y()*(pixelNr/width+0.5) );

	osg::Vec4f fragColor( -1.0f, -1.0f, -1.0f, -1.0f );

	if ( udfLayer && !_dummyTexture )
	    udf = udfLayer->getTextureVec(globalCoord)[udfChannel];

	if ( udf<1.0 )
	{
	    for ( it=_processList.rbegin(); it!=_processList.rend(); it++ )
	    {
		(*it)->doProcess( fragColor, udf, globalCoord );

		if ( fragColor[3]>=1.0f )
		    break;
	    }

	    if ( _dummyTexture )
		fragColor = osg::Vec4f( 0.0f, 0.0f, 0.0f, 0.0f );
	    else if ( fragColor[0]==-1.0f )
		fragColor = osg::Vec4f( 1.0f, 1.0f, 1.0f, _minOpacity );
	}

	if ( udf>=1.0f )
	    fragColor = udfColor;
	else if ( udf>0.0 )
	{
	    if ( udfColor[3]<=0.0f )
		fragColor[3] *= 1.0f-udf;
	    else if ( udfColor[3]>=1.0f && fragColor[3]>=1.0f )
		fragColor = fragColor*(1.0f-udf) + udfColor*udf;
	    else if ( fragColor[3]>0.0f )
	    {
		const float a = fragColor[3]*(1.0f-udf);
		const float b = udfColor[3]*udf;
		fragColor = (fragColor*a + udfColor*b) / (a+b);
		fragColor[3] = a+b;
	    }
	    else
	    {
		fragColor = udfColor;
		fragColor[3] *= udf;
	    }
	}

	if ( fragColor[3]<0.5f/255.0f )
	    fragColor = osg::Vec4f( 0.0f, 0.0f, 0.0f, 0.0f );

	if ( pixelNr<nrImagePixels )
	{
	    fragColor *= 255.0f;
	    for ( int tc=0; tc<4; tc++ )
	    {
		int val = (int) floor( fragColor[tc]+0.5 );
		val = val<=0 ? 0 : (val>=255 ? 255 : val);

		*imagePtr = (unsigned char) val;
		imagePtr++;
	    }
	}
	else
	    _borderColor = fragColor;
    }

    _readyCount.completed();
}


void LayeredTexture::createCompositeTexture( bool dummyTexture )
{
    if ( !_compositeLayerUpdate )
	return;

    _compositeLayerUpdate = false;
    updateTilingInfoIfNeeded();
    const osgGeo::TilingInfo& ti = *_tilingInfo;

    int width  = (int) ceil( ti._envelopeSize.x()/ti._smallestScale.x() );
    int height = (int) ceil( ti._envelopeSize.y()/ti._smallestScale.y() );

    if ( dummyTexture || width<1 )
	width = 1;
    if ( dummyTexture || height<1 )
	height = 1;

    const int idx = getDataLayerIndex( _compositeLayerId );

    _dataLayers[idx]->_origin = ti._envelopeOrigin;
    _dataLayers[idx]->_scale = osg::Vec2f( ti._envelopeSize.x()/float(width),
					   ti._envelopeSize.y()/float(height) );

    osg::Image* image = const_cast<osg::Image*>(_dataLayers[idx]->_image.get());

    if ( !image || width!=image->s() || height!=image->t() )
    {
	image = new osg::Image;
	image->allocateImage( width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE );
    }

    std::vector<LayerProcess*> processList;
    float minOpacity = 1.0f;

    std::vector<LayerProcess*>::const_iterator it = _processes.begin();
    for ( ; _isOn && it!=_processes.end(); it++ )
    {
	if ( (*it)->getTransparencyType()!=FullyTransparent )
	    processList.push_back( *it );

	if ( (*it)->getOpacity() < minOpacity )
	    minOpacity = (*it)->getOpacity();
    }

    int nrPixels = height*width;

    /* Cannot cover mixed use of uniform and extended-edge-pixel borders
       without shaders (trick with extra one-pixel wide border is screwed
       by mipmapping) */
    osg::Vec4f borderColor = getDataLayerBorderColor( _compositeLayerId );
    if ( borderColor[0]>=0.0f )	
	nrPixels++; // One extra pixel to compute uniform composite borderColor		
    int nrTasks = OpenThreads::GetNumberOfProcessors();

    if ( nrTasks<1 )
	 nrTasks=1;
    if ( nrTasks>nrPixels )
	nrTasks = nrPixels;

    std::vector<osg::ref_ptr<CompositeTextureTask> > tasks;
    OpenThreads::BlockCount readyCount( nrTasks );
    readyCount.reset();

    int remainder = nrPixels%nrTasks;
    int start = 0;

    while ( start<nrPixels )
    {
	int stop = start + nrPixels/nrTasks;
	if ( remainder )
	    remainder--;
	else
	    stop--;

	osg::ref_ptr<CompositeTextureTask> task = new CompositeTextureTask( *this, *image, borderColor, processList, minOpacity, dummyTexture, start, stop, readyCount );

	tasks.push_back( task.get() );
	task->start();

	start = stop+1;
    }

    readyCount.block();

    const bool retilingNeededAlready = _tilingInfo->_retilingNeeded;

    setDataLayerImage( _compositeLayerId, image );

    setUpdateVar( _retileCompositeLayer, 
		  _tilingInfo->_needsUpdate ||
		  getDataLayerTextureUnit(_compositeLayerId)!=0 ||
		  borderColor!=getDataLayerBorderColor(_compositeLayerId) );

    if ( _reInitTiling || _useShaders )
	setUpdateVar( _retileCompositeLayer, false );

    setDataLayerBorderColor( _compositeLayerId, borderColor );

    setUpdateVar( _tilingInfo->_needsUpdate, false );
    setUpdateVar( _tilingInfo->_retilingNeeded, retilingNeededAlready );
}


const osg::Image* LayeredTexture::getCompositeTextureImage()
{
    createCompositeTexture();
    return getDataLayerImage( _compositeLayerId );
}


} //namespace
