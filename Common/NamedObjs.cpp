//---------------------------------------------------------------------------
#include "stdafx.h"

#include "NamedObjs.h"
#include "Common.h"
//---------------------------------------------------------------------------
int NamedObjectSortProc(void * Item1, void * Item2)
{
  bool HasPrefix1 = TNamedObjectList::IsHidden((TNamedObject *)Item1);
  bool HasPrefix2 = TNamedObjectList::IsHidden((TNamedObject *)Item2);
  if (HasPrefix1 && !HasPrefix2) return -1;
    else
  if (!HasPrefix1 && HasPrefix2) return 1;
    else
  return ::AnsiCompareStr(((TNamedObject *)Item1)->Name, ((TNamedObject *)Item2)->Name);
}
//--- TNamedObject ----------------------------------------------------------
int TNamedObject::CompareName(std::wstring aName,
  bool CaseSensitive)
{
  if (CaseSensitive)
    return ::AnsiCompare(Name, aName);
  else
    return ::AnsiCompareIC(Name, aName);
}
//---------------------------------------------------------------------------
void TNamedObject::MakeUniqueIn(TNamedObjectList * List)
{
  // This object can't be item of list, it would create infinite loop
  if (List && (List->IndexOf(this) == -1))
    while (List->FindByName(Name))
    {
      int N = 0, P;
      // If name already contains number parenthesis remove it (and remember it)
      if ((Name[Name.size()] == L')') && ((P = ::LastDelimiter(Name, L"(")) > 0))
        try {
          N = StrToInt(Name.substr(P + 1, Name.size() - P - 1));
          Name.erase(P, Name.size() - P + 1);
          Name = ::TrimRight(Name);
        } catch (exception &E) { N = 0; };
      Name += L" (" + IntToStr(N+1) + L")";
    }
}
//--- TNamedObjectList ------------------------------------------------------
const std::wstring TNamedObjectList::HiddenPrefix = L"_!_";
//---------------------------------------------------------------------------
bool TNamedObjectList::IsHidden(TNamedObject * Object)
{
  return (Object->Name.substr(1, HiddenPrefix.size()) == HiddenPrefix);
}
//---------------------------------------------------------------------------
TNamedObjectList::TNamedObjectList():
  TObjectList()
{
  AutoSort = true;
}
//---------------------------------------------------------------------------
TNamedObject * TNamedObjectList::AtObject(int Index)
{
  return (TNamedObject *)GetItem(Index+GetHiddenCount());
}
//---------------------------------------------------------------------------
void TNamedObjectList::Recount()
{
  int i = 0;
  while ((i < TObjectList::GetCount()) && IsHidden((TNamedObject *)GetItem(i))) i++;
  FHiddenCount = i;
}
//---------------------------------------------------------------------------
void TNamedObjectList::AlphaSort()
{
  Sort(NamedObjectSortProc);
}
//---------------------------------------------------------------------------
void TNamedObjectList::Notify(void *Ptr, TListNotification Action)
{
  TObjectList::Notify(Ptr, Action);
  if (AutoSort && (Action == lnAdded)) AlphaSort();
  Recount();
}
//---------------------------------------------------------------------------
TNamedObject * TNamedObjectList::FindByName(std::wstring Name,
  bool CaseSensitive)
{
  for (int Index = 0; Index < TObjectList::Count; Index++)
    if (!((TNamedObject *)GetItem(Index))->CompareName(Name, CaseSensitive))
      return (TNamedObject *)GetItem(Index);
  return NULL;
}
//---------------------------------------------------------------------------
void TNamedObjectList::SetCount(int value)
{
  TObjectList::SetCount(value/*+HiddenCount*/);
}
//---------------------------------------------------------------------------
int TNamedObjectList::GetCount()
{
  return TObjectList::Count - HiddenCount;
}
