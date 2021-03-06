#include "SSNetDataLoader.h"

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/ndarrayobject.h>

#include <cstdio>
#include <time.h>

#include "larcv/core/DataFormat/EventImage2D.h"

namespace ssnet {


  bool SSNetDataLoader::_setup_numpy = false;

  SSNetDataLoader::~SSNetDataLoader()
  {
    delete _io;
    delete _rand;
  }
  
  void SSNetDataLoader::setup( std::string data_file_path, int ran_seed, bool with_truth )
  {
    if ( _reverse_time_order )
      _io = new larcv::IOManager( larcv::IOManager::kREAD, "ssnetio", larcv::IOManager::kTickBackward );
    else
      _io = new larcv::IOManager( larcv::IOManager::kREAD, "ssnetio", larcv::IOManager::kTickForward );
    
    _io->add_in_file( data_file_path );
    _io->specify_data_read( larcv::kProductImage2D, _adc_image_treename );    
    if ( with_truth )
      _io->specify_data_read( larcv::kProductImage2D, _truth_image_treename );
    if ( _reverse_time_order )
      _io->reverse_all_products();
    _io->initialize();
    _num_entries = _io->get_n_entries();
    _rand = new TRandom3( ran_seed );
    _io->read_entry( 0 );

    // need to know image sizes
    larcv::EventImage2D* ev_pixval = (larcv::EventImage2D*)_io->get_data( larcv::kProductImage2D, _adc_image_treename );
    auto const& adc_v = ev_pixval->as_vector();

    for (int i=0; i<(int)adc_v.size(); i++) {
      LARCV_NORMAL() << "meta: " << adc_v.at(i).meta().dump() << std::endl;
      if ( _colsize==0 ) {
        _colsize = (int)adc_v.at(i).meta().cols();
        _rowsize = (int)adc_v.at(i).meta().rows();
      }
      else {
        if ( _colsize!=(int)adc_v.at(i).meta().cols() || _rowsize!=(int)adc_v.at(i).meta().rows() ) {
          throw std::runtime_error("The col and rows of the images are not the same for the different planes!");
        }
      }
    }
    _numplanes = (int)adc_v.size();
    LARCV_NORMAL() << "Ready to return images with (row,col) size of (" << _rowsize << "," << _colsize << ")" << std::endl;
    resetTimers();    
  }
  
  PyObject* SSNetDataLoader::makeTrainingDataDict( int batchsize, int plane )
  {


    if ( !SSNetDataLoader::_setup_numpy ) {
      import_array1(0);
      SSNetDataLoader::_setup_numpy = true;
    }

    if ( !_io ) {
      Py_INCREF(Py_None);
      return Py_None;
    }
    

    // DECLARE TENSORS and dict keys

    int numplanes = (plane<0 ) ? _numplanes : 1;
    if ( numplanes==1 &&  plane>=_numplanes ) {
      LARCV_WARNING() << "Invalid plane requested (" << plane << "). Should be between [0," << _numplanes << ")" << std::endl;
      Py_INCREF(Py_None);
      return Py_None;
    }

    std::clock_t t_alloc_start = clock();
    
    // image tensor in pytorch format: (batch,numchannels,height,width)
    // npixels returned
    npy_intp img_t_dim[] = { (long int)batchsize, (long int)numplanes, (long int)_rowsize, (long int)_colsize };
    PyArrayObject* img_t = (PyArrayObject*)PyArray_SimpleNew( 4, img_t_dim, NPY_FLOAT );
    PyObject *img_t_key = Py_BuildValue("s", "image_t");        

    npy_intp lab_t_dim[] = { (long int)batchsize, (long int)numplanes, (long int)_rowsize, (long int)_colsize };
    PyArrayObject* lab_t = (PyArrayObject*)PyArray_SimpleNew( 4, lab_t_dim, NPY_FLOAT );
    PyObject *lab_t_key = Py_BuildValue("s", "label_t");
    
    PyObject *d = PyDict_New();

    std::vector< int > plane_v(numplanes,0);
    if ( numplanes==1 )
      plane_v[0] = plane;
    else
      for (int i=0; i<numplanes; i++) plane_v[i] = i;

    if ( sizeof(float)!=4 ) {
      throw std::runtime_error("This function assumes float in image2d is same size of NPY_FLOAT which is 32-bit or 4 bytes");
    }

   const size_t npixels = _rowsize*_colsize;

   std::set<int> sub_batch_indices;
   if ( _read_mode==kRandomSubBatch ) {
     while ( (int)sub_batch_indices.size()<batchsize ) {
       int idx = _rand->Integer( batchsize*_sub_batch_factor );
       sub_batch_indices.insert(idx);
     }
   }

   float dt_alloc = float(clock()-t_alloc_start)/CLOCKS_PER_SEC;
   _timer_alloc += dt_alloc;


   std::clock_t t_start_read;
   std::clock_t t_start_copy;
   float dt_read = 0.;
   float dt_copy = 0.;

   
   int NB = batchsize;
   if ( _read_mode==kRandomSubBatch )
     NB = batchsize*_sub_batch_factor;

   int ibatch_fill_index = 0;
   for (int ibatch=0; ibatch<NB; ibatch++) {
     //std::cout << "ibatch: " << ibatch << std::endl;
     
     t_start_read = std::clock();

     bool do_copy = true;
       
     if ( _read_mode==kSequential ) {
       _io->read_entry( _current_entry );
       _current_entry++;
       if ( _current_entry==_num_entries )
         _current_entry = 0;
       ibatch_fill_index = ibatch;
     }
     else if ( _read_mode==kCompleteRandom ) {
       _current_entry = _rand->Integer( _num_entries );
       _io->read_entry( _current_entry );
       ibatch_fill_index = ibatch;       
     }
     else if ( _read_mode==kPartialRandom || _read_mode==kRandomSubBatch ) {
       if ( ibatch==0 )
         _current_entry = _rand->Integer( _num_entries );

       if ( _read_mode==kRandomSubBatch ) {
         // if ibatch not in sub_batch_indices, don't fill this entry into the output tensor
         if ( sub_batch_indices.find( ibatch )==sub_batch_indices.end() )
           do_copy = false;
       }

       if ( do_copy )
         _io->read_entry( _current_entry );
       _current_entry++;
       if ( _current_entry==_num_entries )
         _current_entry = 0;
     }
     


     if ( !do_copy ) {
       dt_read += float(clock()-t_start_read);       
       continue;
     }
       
     //std::cout << "  fill tensor, batch-index=" << ibatch_fill_index << std::endl;
     larcv::EventImage2D* ev_pixval = (larcv::EventImage2D*)_io->get_data( larcv::kProductImage2D, _adc_image_treename );
     larcv::EventImage2D* ev_labels = (larcv::EventImage2D*)_io->get_data( larcv::kProductImage2D, _truth_image_treename );
     dt_read += float(clock()-t_start_read);            
     
     // get the data in std::vector format
     t_start_copy = std::clock();
     int ip=0; 
     for (auto const& p : plane_v ) {       
       
       // fast using memcpy
       // // get the raw pixel data
       auto const& vec_img = ev_pixval->as_vector()[p].as_vector();
       float* p_imgdata = (float*)PyArray_DATA( img_t );
       auto const& vec_label = ev_labels->as_vector()[p].as_vector();
       float* p_labdata = (float*)PyArray_DATA( lab_t );

       // dangerous       
       size_t offset = numplanes*npixels*ibatch_fill_index + npixels*ip;
       memcpy( p_imgdata+offset, vec_img.data(), sizeof(float)*npixels );
       memcpy( p_labdata+offset, vec_label.data(), sizeof(float)*npixels );       
       
       // slow using assignment and access operators
       // auto const& img2d = ev_pixval->as_vector()[p];
       // auto const& lab2d = ev_labels->as_vector()[p];
       // for (int c=0; c<_colsize; c++) {
       //   for (int r=0; r<_rowsize; r++) {
       //     *((float*)PyArray_GETPTR4( img_t, ibatch_fill_index, ip, r, c )) = img2d.pixel(r,c,__FILE__,__LINE__);
       //     *((float*)PyArray_GETPTR4( lab_t, ibatch_fill_index, ip, r, c )) = lab2d.pixel(r,c,__FILE__,__LINE__);            
       //   }
       // }
       
       ip++;
     }//end of plane loop

     dt_copy += float(std::clock()-t_start_copy);     
     ibatch_fill_index++;

   }//end of batch loop


   if ( _read_mode==kRandomSubBatch && ibatch_fill_index!=batchsize ) {
     throw std::runtime_error("Did not fill batch completely");
   }

   //std::cout << "dt_read: " << dt_read << std::endl;
   //std::cout << "dt_copy: " << dt_copy << std::endl;
   
   _timer_ioman_read += dt_read/(float)CLOCKS_PER_SEC;
   _timer_datacopy += dt_copy/(float)CLOCKS_PER_SEC;
   
    // add to dict
    PyDict_SetItem(d, img_t_key, (PyObject*)img_t);
    PyDict_SetItem(d, lab_t_key, (PyObject*)lab_t);    

    // dereference key and arrays since we are giving it to the dict
    Py_DECREF(img_t_key);
    Py_DECREF(img_t);
    Py_DECREF(lab_t_key);
    Py_DECREF(lab_t);
    
    return (PyObject*)d;
  }

}
