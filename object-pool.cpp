#include <iostream>
#include <array>
#include <assert.h>

enum ObjectId {
  CIRCLE_BEGIN = 0,
  CIRCLE_END = 100,
  RECTANGLE_BEGIN = 100,
  RECTANGLE_END = 200
};

struct Point {
  float _x;
  float _y;
};

struct Circle {
  ObjectId _id;
  Point _center;
  float _radius;
  
  Circle(ObjectId id) :
    _id(id) {
    // std::cout << __PRETTY_FUNCTION__ << " id = " << id << std::endl;
  }
};

struct Rectangle {
  ObjectId _id;
  Point _lowerLeft;
  Point _upperRight;
  
  Rectangle(ObjectId id) :
    _id(id) {
    // std::cout << __PRETTY_FUNCTION__ << " id = " << id << std::endl;
  }
};

template <class T, size_t BEGIN, size_t END, size_t IDX, bool recur = (BEGIN < END)>
struct ObjectPoolImpl;

template <class T, size_t BEGIN, size_t END, size_t IDX>
struct ObjectPoolImpl<T, BEGIN, END, IDX, false> {
  template <size_t N>
  void fillStack(std::array<T*, N>&) {
    // std::cout << __PRETTY_FUNCTION__ << std::endl;
  }
};

// So here I'm using inheritance instead of initialization lists, which maybe people find more
// digestive. We have one level of inheritance for one instance of object in the pool.
template <class T, size_t BEGIN, size_t END, size_t IDX>
struct ObjectPoolImpl<T, BEGIN, END, IDX, true> : public ObjectPoolImpl<T, BEGIN+1, END, IDX+1> {
  typedef ObjectPoolImpl<T, BEGIN+1, END, IDX+1> BaseClass;

  T _obj;

  ObjectPoolImpl() :
    _obj(static_cast<ObjectId>(BEGIN)) { }

  template <size_t N>
  void fillStack(std::array<T*, N>& arr) {
    // std::cout << __PRETTY_FUNCTION__ << std::endl;
    arr[IDX] = &_obj;
    BaseClass::fillStack(arr);
  }
};

template <class T, size_t BEGIN, size_t END>
struct ObjectPool {
  static const size_t COUNT = END - BEGIN;

  ObjectPoolImpl<T, BEGIN, END, 0> _impl;
  std::array<T*, COUNT> _stack;
  size_t _stackPtr = 0;

  ObjectPool() {
    _impl.fillStack(_stack);
  }

  T* pop() {
    return _stackPtr < COUNT ? _stack[_stackPtr++] : NULL;
  }

  void push() {
    assert(_stackPtr > 0);
    --_stackPtr;
  }
};

int main() {
  ObjectPool<Circle, CIRCLE_BEGIN, CIRCLE_END> circles;
  ObjectPool<Rectangle, RECTANGLE_BEGIN, RECTANGLE_END> rectangles;

  return 0;
}
